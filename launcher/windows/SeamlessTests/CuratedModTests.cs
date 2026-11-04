using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using BloodborneLauncher.Core;

/// <summary>The curated mod Nocturne: content hash (against tools/mod-release.py), manifest, install from a local zip, game
/// mode in mods.json, join advice and file clashes. No network.</summary>
internal static class CuratedModTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void Put(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, System.Text.Encoding.UTF8.GetBytes(text));
    }

    private static string Sha(string path)
    {
        using var s = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(s)).ToLowerInvariant();
    }

    private static string Manifest(string schema = "1", string url = "https://example.org/n.zip", string sha = "", bool dropSha = false)
    {
        var hex = new string('a', 64);
        return "{\"schema\":" + schema + ",\"name\":\"Nocturne\",\"page\":\"\",\"latest\":\"1.3\",\"versions\":[{\"version\":\"1.3\",\"url\":\""
            + url + "\"," + (dropSha ? "" : "\"sha256\":\"" + (sha.Length > 0 ? sha : hex) + "\",") + "\"size\":10,\"content\":\"" + hex + "\"}]}";
    }

    private static bool Throws<T>(Action action) where T : Exception
    {
        try { action(); }
        catch (T) { return true; }
        return false;
    }

    private static async Task<bool> ThrowsAsync<T>(Func<Task> action) where T : Exception
    {
        try { await action(); }
        catch (T) { return true; }
        return false;
    }

    internal static void Run() => RunAsync().GetAwaiter().GetResult();

    private static async Task RunAsync()
    {
        var tmp = Path.Combine(Path.GetTempPath(), "nw-curated-" + Guid.NewGuid().ToString("N"));
        var oldData = Environment.GetEnvironmentVariable("BB_DATA_DIR");
        try
        {
            // ---- content hash: the vector made by tools/mod-release.py
            const string vector = "b85ed0c5739f633a067399da4ae87ff72638cd9f6355cd95a95bbe2ffc8b40f4";
            var plain = Path.Combine(tmp, "hash", "Nocturne");
            Put(Path.Combine(plain, "dvdroot_ps4", "param", "a.bin"), "x\n");
            Put(Path.Combine(plain, "readme.txt"), "hi\n");
            Check(CuratedMods.ContentHash(plain) == vector, "Content hash differs from tools/mod-release.py: " + CuratedMods.ContentHash(plain));
            var wrapped = Path.Combine(tmp, "hash2", "Nocturne");
            Put(Path.Combine(wrapped, "app0", "dvdroot_ps4", "param", "a.bin"), "x\n");
            Put(Path.Combine(wrapped, "readme.txt"), "hi\n");
            Put(Path.Combine(wrapped, "nocturne.json"), "{}");
            Check(CuratedMods.ContentHash(wrapped) == vector, "An app0 wrapper or nocturne.json changed the content hash");
            Check(Throws<InvalidDataException>(() => CuratedMods.ContentHash(Path.Combine(tmp, "hash"))) == false,
                "A folder above the mod must still hash (its subfolder holds dvdroot_ps4)");

            // ---- manifest
            var good = CuratedMods.ParseManifest(Manifest());
            Check(good.Latest == "1.3" && good.Versions.Count == 1 && good.Newest.Size == 10, "Good manifest not read");
            Check(Throws<FormatException>(() => CuratedMods.ParseManifest(Manifest(url: "http://example.org/n.zip"))), "An http url was accepted");
            Check(Throws<FormatException>(() => CuratedMods.ParseManifest(Manifest(schema: "2"))), "Schema 2 was accepted");
            Check(Throws<FormatException>(() => CuratedMods.ParseManifest(Manifest(dropSha: true))), "A missing sha256 was accepted");
            Check(Throws<FormatException>(() => CuratedMods.ParseManifest("not json")), "Garbage was accepted as a manifest");
            Check(CuratedMods.ManifestUrl.StartsWith("https://") || Environment.GetEnvironmentVariable("BB_NOCTURNE_MANIFEST") != null,
                "The default manifest address is not https");

            // ---- install from a local zip (no network)
            var source = Path.Combine(tmp, "src");
            Put(Path.Combine(source, "dvdroot_ps4", "param", "gameparam.parambnd.dcx"), "param-1.3");
            Put(Path.Combine(source, "dvdroot_ps4", "chr", "c1000.anibnd.dcx"), "chr-1.3");
            Put(Path.Combine(source, "readme.txt"), "notes");
            var zipPath = Path.Combine(tmp, "nocturne-1.3.zip");
            ZipFile.CreateFromDirectory(source, zipPath);
            var entry = new ModVersion("1.3", "https://example.org/nocturne-1.3.zip", Sha(zipPath), new FileInfo(zipPath).Length, CuratedMods.ContentHash(source));
            CuratedMods.Fetch local(string file) => (_, to, _, _) => { File.Copy(file, to, true); return Task.CompletedTask; };

            var modsDir = Path.Combine(tmp, "data", "mods");
            Put(Path.Combine(modsDir, "Nocturne", "dvdroot_ps4", "old.bin"), "old");
            Put(Path.Combine(modsDir, "Other", "param", "x.bin"), "other");
            var wrong = entry with { Sha256 = new string('0', 64) };
            Check(await ThrowsAsync<InvalidDataException>(() => CuratedMods.InstallAsync(wrong, modsDir, null, CancellationToken.None, local(zipPath))),
                "A download with the wrong sha256 was installed");
            Check(File.Exists(Path.Combine(modsDir, "Nocturne", "dvdroot_ps4", "old.bin")), "A failed install touched the old folder");
            Check(!Directory.GetFileSystemEntries(modsDir).Any(e => Path.GetFileName(e).StartsWith('.')), "A failed install left temporary folders");
            var wrongSize = entry with { Size = entry.Size + 1 };
            Check(await ThrowsAsync<InvalidDataException>(() => CuratedMods.InstallAsync(wrongSize, modsDir, null, CancellationToken.None, local(zipPath))),
                "A download of the wrong size was installed");
            var wrongContent = entry with { Content = new string('1', 64) };
            Check(await ThrowsAsync<InvalidDataException>(() => CuratedMods.InstallAsync(wrongContent, modsDir, null, CancellationToken.None, local(zipPath))),
                "An install with the wrong content hash was kept");
            Check(File.Exists(Path.Combine(modsDir, "Nocturne", "dvdroot_ps4", "old.bin")), "A content mismatch touched the old folder");

            var evilZip = Path.Combine(tmp, "evil.zip");
            using (var z = ZipFile.Open(evilZip, ZipArchiveMode.Create))
            {
                using (var w = new StreamWriter(z.CreateEntry("dvdroot_ps4/param/a.bin").Open())) w.Write("x");
                using (var w = new StreamWriter(z.CreateEntry("../evil").Open())) w.Write("evil");
            }
            var evil = entry with { Sha256 = Sha(evilZip), Size = new FileInfo(evilZip).Length };
            Check(await ThrowsAsync<InvalidDataException>(() => CuratedMods.InstallAsync(evil, modsDir, null, CancellationToken.None, local(evilZip))),
                "A zip with a ../ entry was extracted");
            Check(!File.Exists(Path.Combine(modsDir, "evil")) && !File.Exists(Path.Combine(tmp, "data", "evil")), "The ../evil entry was written");
            Check(File.Exists(Path.Combine(modsDir, "Nocturne", "dvdroot_ps4", "old.bin")), "A refused zip touched the old folder");

            double last = 0;
            await CuratedMods.InstallAsync(entry, modsDir, new Progress<double>(p => last = p), CancellationToken.None, local(zipPath));
            Check(!Directory.Exists(Path.Combine(modsDir, "Nocturne")) && Directory.Exists(Path.Combine(modsDir, "Nocturne v1.3")), "The old folder was not replaced by Nocturne v1.3");
            Check(File.ReadAllText(Path.Combine(modsDir, "Nocturne v1.3", "dvdroot_ps4", "param", "gameparam.parambnd.dcx")) == "param-1.3", "The new files are not in place");
            Check(File.ReadAllText(Path.Combine(modsDir, "Nocturne v1.3", "nocturne.json")).Contains("\"version\": \"1.3\""), "nocturne.json was not written");
            Check(!Directory.GetFileSystemEntries(modsDir).Any(e => Path.GetFileName(e).StartsWith('.')), "The install left temporary folders");
            Check(File.Exists(Path.Combine(modsDir, "Other", "param", "x.bin")), "The install touched another mod");
            var installed = CuratedMods.Installed(modsDir);
            Check(installed.Kind == InstalledKind.Known && installed.Version == "1.3", "The fresh install is not recognised as 1.3");
            _ = last;

            // A hand copy without nocturne.json is matched by content; an edited one is unrecognised.
            File.Delete(Path.Combine(modsDir, "Nocturne v1.3", "nocturne.json"));
            var manifest = new ModManifest("Nocturne", "", "1.3", new[] { entry });
            Check(CuratedMods.Installed(modsDir, manifest) is { Kind: InstalledKind.Known, Version: "1.3" }, "A hand install was not matched to the manifest");
            Check(CuratedMods.Installed(modsDir).Kind == InstalledKind.Unrecognised, "A hand install without a manifest should be unrecognised");
            Put(Path.Combine(modsDir, "Nocturne v1.3", "dvdroot_ps4", "param", "gameparam.parambnd.dcx"), "edited");
            Check(CuratedMods.Installed(modsDir, manifest).Kind == InstalledKind.Unrecognised, "An edited install was matched");
            Check(CuratedMods.Installed(Path.Combine(tmp, "nowhere")).Kind == InstalledKind.None, "A missing folder is not 'none'");

            // ---- Apply: mods.json per game mode
            var applyData = Path.Combine(tmp, "apply");
            var applyMods = Path.Combine(applyData, "mods");
            Environment.SetEnvironmentVariable("BB_DATA_DIR", applyData);
            foreach (var n in new[] { "ModA", "ModB", "Nocturne" })
                Put(Path.Combine(applyMods, n, "dvdroot_ps4", "param", n + ".bin"), n);
            Mods.Save(new[] { new Mods.Entry("ModA", true), new Mods.Entry("ModB", false), new Mods.Entry("Nocturne", false) });
            var s = new LauncherSettings();
            s.Set("mods_enabled", true); // Load's defaults; a bare LauncherSettings has none
            Check(CuratedMods.Mode(s) == "my", "The default game mode is not My mods");
            s.Set("mods_enabled", false);
            Check(CuratedMods.Mode(s) == "standard", "The old Load mods switch off did not read as No mods");
            s.Set("mod_profile", "my");
            Check(CuratedMods.Mode(s) == "standard", "My mods with the old switch off did not read as No mods");
            s.Set("mods_enabled", true);
            Check(CuratedMods.Mode(s) == "my", "My mods with the switch on is not My mods");
            CuratedMods.Apply(s, applyMods);
            Check(Mods.Load(applyMods).Where(m => m.Enabled).Select(m => m.Name).SequenceEqual(new[] { "ModA" }), "My mods mode changed the list");
            s.Set("mod_profile", "nocturne");
            CuratedMods.Apply(s, applyMods);
            Check(Mods.Load(applyMods).Where(m => m.Enabled).Select(m => m.Name).SequenceEqual(new[] { "Nocturne" }), "Nocturne mode did not enable only Nocturne");
            Put(Path.Combine(applyMods, "ModC", "param", "c.bin"), "c");
            CuratedMods.Apply(s, applyMods);
            Check(Mods.Load(applyMods).Where(m => m.Enabled).Select(m => m.Name).SequenceEqual(new[] { "Nocturne" }), "A new folder was enabled in Nocturne mode");
            s.Set("mod_profile", "standard");
            CuratedMods.Apply(s, applyMods);
            Check(!Mods.Load(applyMods).Any(m => m.Enabled), "Standard mode left a mod enabled");
            s.Set("mod_profile", "my");
            CuratedMods.Apply(s, applyMods);
            var back = Mods.Load(applyMods).ToDictionary(m => m.Name, m => m.Enabled);
            Check(back["ModA"] && !back["ModB"] && !back["Nocturne"], "My mods list was not restored");
            Check(back["ModC"], "A folder added meanwhile should join My mods enabled, as Mods.Load treats new folders");
            Check(CuratedMods.MyModsCount(s, applyMods) == 2, "My mods count wrong");
            Directory.Delete(applyMods, true);
            Check(CuratedMods.Apply(new LauncherSettings(), applyMods) == false, "Apply must do nothing without a mods folder");

            // ---- folder name: "Nocturne v<version>" for the launcher's own install; mods.json and the saved list follow it
            Check(CuratedMods.FolderFor("1.0.1") == "Nocturne v1.0.1", "Folder name for 1.0.1");
            Check(CuratedMods.FolderFor("../x") == "Nocturne" && CuratedMods.FolderFor("1.") == "Nocturne" && CuratedMods.FolderFor("") == "Nocturne",
                "An unsafe version must not become a folder name");
            Check(CuratedMods.IsNocturneName("nocturne V1.0") && CuratedMods.IsNocturneName("Nocturne") && !CuratedMods.IsNocturneName("Nocturnes"),
                "Nocturne folder names");
            var nameData = Path.Combine(tmp, "name");
            var nameMods = Path.Combine(nameData, "mods");
            Environment.SetEnvironmentVariable("BB_DATA_DIR", nameData);
            Put(Path.Combine(nameMods, "ModA", "param", "a.bin"), "a");
            var oldNocturne = Path.Combine(nameMods, "Nocturne");
            Put(Path.Combine(oldNocturne, "dvdroot_ps4", "param", "n.bin"), "n");
            File.WriteAllText(Path.Combine(oldNocturne, "nocturne.json"),
                "{\"version\":\"1.0\",\"content\":\"" + CuratedMods.ContentHash(oldNocturne) + "\"}");
            Mods.Save(new[] { new Mods.Entry("Nocturne", false), new Mods.Entry("ModA", true) });
            var ns = new LauncherSettings();
            ns.Set(CuratedMods.SavedKey, "{\"order\":[\"ModA\",\"Nocturne\"],\"disabled\":[\"Nocturne\"]}");
            Check(CuratedMods.NameFolder(ns, nameMods), "Renaming must report the changed saved list");
            Check(!Directory.Exists(oldNocturne) && Directory.Exists(Path.Combine(nameMods, "Nocturne v1.0")), "The launcher's install was not renamed to Nocturne v1.0");
            Check(Mods.Load(nameMods).SequenceEqual(new[] { new Mods.Entry("Nocturne v1.0", false), new Mods.Entry("ModA", true) }),
                "The rename lost Nocturne's place or its off state in mods.json");
            Check(ns.Str(CuratedMods.SavedKey).Contains("\"Nocturne v1.0\"") && !ns.Str(CuratedMods.SavedKey).Contains("\"Nocturne\""),
                "The saved My mods list still names the old folder");
            Check(CuratedMods.Installed(nameMods) is { Kind: InstalledKind.Known, Version: "1.0" }, "The renamed install is not recognised");
            Check(!CuratedMods.NameFolder(ns, nameMods), "A second rename changed something");
            var handCopy = Path.Combine(nameMods, "Nocturne v1.0");
            File.Delete(Path.Combine(handCopy, "nocturne.json"));
            Directory.Move(handCopy, Path.Combine(nameMods, "Nocturne"));
            CuratedMods.NameFolder(ns, nameMods);
            Check(Directory.Exists(Path.Combine(nameMods, "Nocturne")), "A hand install (no nocturne.json) was renamed");
            Environment.SetEnvironmentVariable("BB_DATA_DIR", applyData);

            // ---- Workshop popup: Try without Nocturne, Update from an older known version, once per manifest version
            var latest13 = CuratedMods.ParseManifest(Manifest());
            Check(CuratedMods.StartNewsOf(latest13, new InstalledMod(InstalledKind.None), "") == StartNews.Try, "No Nocturne: offer it");
            Check(CuratedMods.StartNewsOf(latest13, new InstalledMod(InstalledKind.Known, "1.2"), "") == StartNews.Update, "Older Nocturne: offer the update");
            Check(CuratedMods.StartNewsOf(latest13, new InstalledMod(InstalledKind.Known, "1.3"), "") == StartNews.None, "Newest Nocturne: nothing");
            Check(CuratedMods.StartNewsOf(latest13, new InstalledMod(InstalledKind.Unrecognised), "") == StartNews.None, "Own copy: nothing");
            Check(CuratedMods.StartNewsOf(latest13, new InstalledMod(InstalledKind.None), "1.3") == StartNews.None, "Told about 1.3 already: nothing");
            Check(CuratedMods.StartNewsOf(latest13, new InstalledMod(InstalledKind.Known, "1.2"), "1.2") == StartNews.Update, "A newer version is told again");
            Check(CuratedMods.StartNewsOf(null, new InstalledMod(InstalledKind.None), null) == StartNews.None, "No manifest: nothing");

            // ---- join advice (visy's terms: the label is a key; only a manifest version is fetched; others show "modded")
            var nocturne13 = new LocalMods("nocturne", "1.3", 0);
            var offered = new[] { "1.3", "1.2" };
            Check(CuratedMods.JoinAdvice("nocturne@1.3", nocturne13, offered) == new JoinChoice(JoinKind.Join), "Same Nocturne must join");
            Check(CuratedMods.JoinAdvice("nocturne@1.2", nocturne13, offered) == new JoinChoice(JoinKind.GetNocturne, "1.2"), "An older host Nocturne in the manifest must be fetched");
            Check(CuratedMods.JoinAdvice("nocturne@1.3", new LocalMods("my", null, 0), offered) == new JoinChoice(JoinKind.GetNocturne, "1.3"), "A guest without Nocturne must get it");
            Check(CuratedMods.JoinAdvice("nocturne@1.3", new LocalMods("my", "1.3", 2), null) == new JoinChoice(JoinKind.GetNocturne, "1.3"), "An installed version switches even without the manifest");
            Check(CuratedMods.JoinAdvice("nocturne@9.9", nocturne13, offered) == new JoinChoice(JoinKind.NocturneNotAvailable, "9.9"), "A version not in the manifest is not available");
            Check(CuratedMods.JoinAdvice("nocturne@1.2", nocturne13, null).Kind == JoinKind.NocturneNotAvailable, "No manifest: an uninstalled version is not available");
            Check(CuratedMods.JoinAdvice("nocturne@1.3+extra:3f9a12c0", nocturne13, offered).Kind == JoinKind.Join, "Extra label parts are ignored");
            Check(CuratedMods.JoinAdvice("nocturne@../x", nocturne13, offered).Kind == JoinKind.Modded, "A version with path characters is never a key");
            Check(CuratedMods.JoinAdvice("mods:0a1b2c3d", new LocalMods("my", null, 2), offered).Kind == JoinKind.Modded, "Another label only shows modded");
            Check(CuratedMods.JoinAdvice("mods:0a1b2c3d", new LocalMods("standard", null, 0), offered).Kind == JoinKind.Modded, "Another label only shows modded (vanilla guest)");
            Check(CuratedMods.JoinAdvice("", new LocalMods("my", null, 0)).Kind == JoinKind.Join, "No mods on both sides joins");
            Check(CuratedMods.JoinAdvice(null, nocturne13).Kind == JoinKind.MismatchJoinAnyway, "A guest with Nocturne and a host without mods is a mismatch");
            Check(CuratedMods.NocturneOf("x+nocturne@1.0") == "1.0" && CuratedMods.NocturneOf("Nocturne@1.0") == null, "Label parts");

            // ---- host label
            var labelMods = Path.Combine(tmp, "label-mods");
            var labelSettings = new LauncherSettings();
            Directory.CreateDirectory(Path.Combine(labelMods, "Nocturne", "dvdroot_ps4", "param"));
            File.WriteAllText(Path.Combine(labelMods, "Nocturne", "dvdroot_ps4", "param", "a.bin"), "x\n");
            var labelHash = CuratedMods.ContentHash(Path.Combine(labelMods, "Nocturne"));
            File.WriteAllText(Path.Combine(labelMods, "Nocturne", "nocturne.json"), "{\"version\":\"1.0\",\"content\":\"" + labelHash + "\"}");
            Check(CuratedMods.HostLabel(labelSettings, labelMods) == null, "No label outside Nocturne mode");
            labelSettings.Set(CuratedMods.ProfileKey, "nocturne");
            Check(CuratedMods.HostLabel(labelSettings, labelMods) == "nocturne@1.0", "Nocturne mode with a verified install announces its version");
            File.WriteAllText(Path.Combine(labelMods, "Nocturne", "dvdroot_ps4", "param", "b.bin"), "changed");
            Check(CuratedMods.HostLabel(labelSettings, labelMods) == null, "A changed install announces no Nocturne label");

            // ---- file clashes
            var clash = Path.Combine(tmp, "clash");
            Put(Path.Combine(clash, "A", "dvdroot_ps4", "param", "gameparam.parambnd.dcx"), "a");
            Put(Path.Combine(clash, "A", "dvdroot_ps4", "chr", "only-a.bin"), "a");
            Put(Path.Combine(clash, "B", "param", "gameparam.parambnd.dcx"), "b");
            Put(Path.Combine(clash, "C", "msg", "x.bin"), "c");
            Put(Path.Combine(clash, "C", "readme.txt"), "c");
            Put(Path.Combine(clash, "A", "readme.txt"), "a");
            var clashes = Mods.Conflicts(clash, new[] { "A", "B", "C" });
            Check(clashes.Count == 2 && clashes["A"].Single().With == "B" && clashes["B"].Single().With == "A"
                  && clashes["A"].Single().Examples.SequenceEqual(new[] { "param/gameparam.parambnd.dcx" }), "Clash between A and B not listed");
            Check(Mods.Conflicts(clash, new[] { "A", "C" }).Count == 0 && Mods.Conflicts(clash, new[] { "B", "C" }).Count == 0, "Disjoint mods listed as clashing");
            Check(Mods.Conflicts(clash, new[] { "A" }).Count == 0, "A single mod clashes with itself");
        }
        finally
        {
            Environment.SetEnvironmentVariable("BB_DATA_DIR", oldData);
            try { Directory.Delete(tmp, true); } catch (IOException) { }
        }
    }
}
