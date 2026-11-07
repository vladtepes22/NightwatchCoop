using System.IO;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using BloodborneLauncher.Core;

/// <summary>The randomized-world join flow: the C# world-code decoder against the generator's own vectors
/// (tools/randomizer/tests/r2_vectors.json, the contract), the lobby label parts, the join advice, world mode's mods.json
/// and the host label. No generator is started.</summary>
internal static class WorldJoinTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException("World join: " + message);
    }

    internal static string RepoFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var path = Path.Combine(dir.FullName, relative);
            if (File.Exists(path)) return path;
        }
        throw new FileNotFoundException("not found above the test folder: " + relative);
    }

    /// <summary>The generator's code vectors (tools/randomizer/tests/r2_vectors.json), or null in a checkout without the
    /// private generator (the public branch): the checks that need them are skipped there and run in the generator's tree.</summary>
    internal static JsonNode? GeneratorVectors()
    {
        try { return JsonNode.Parse(File.ReadAllText(RepoFile(Path.Combine("tools", "randomizer", "tests", "r2_vectors.json"))))!; }
        catch (FileNotFoundException)
        {
            Console.WriteLine("SKIP: world-code checks against the generator's vectors (no tools/randomizer in this checkout)");
            return null;
        }
    }

    private static bool SameOption(object mine, JsonNode? theirs) => mine switch
    {
        long v => theirs is JsonValue j && j.GetValue<long>() == v,
        long[] list => theirs is JsonArray a && a.Select(n => n!.GetValue<long>()).SequenceEqual(list),
        ValueTuple<long, long>[] pairs => theirs is JsonArray a && a.Count == pairs.Length &&
            a.Select((n, i) => n is JsonArray p && p[0]!.GetValue<long>() == pairs[i].Item1 && p[1]!.GetValue<long>() == pairs[i].Item2).All(x => x),
        _ => false,
    };

    internal static void Run()
    {
        // ---- the decoder against the generator's vectors
        if (GeneratorVectors() is not { } vectors) return; // the rest uses codes from the vectors
        Check(vectors["schema"]!.GetValue<int>() == WorldCode.Schema && vectors["core_bits"]!.GetValue<int>() == WorldCode.CoreBits, "schema / core bits differ from the vectors");
        string? sample = null, nocturneSample = null;
        foreach (var v in vectors["vectors"]!.AsArray())
        {
            var name = v!["name"]!.GetValue<string>();
            var result = WorldCode.Decode(v["code"]!.GetValue<string>());
            if (v["error"] is { } error)
            {
                var want = error.GetValue<string>() == "newer" ? WorldCodeKind.Newer : WorldCodeKind.Bad;
                Check(result.Kind == want, $"vector '{name}': {result.Kind}, expected {want}");
                continue;
            }
            Check(result.Kind == WorldCodeKind.Ok, $"vector '{name}' refused");
            Check(result.Canonical == v["canonical"]!.GetValue<string>(), $"vector '{name}': canonical {result.Canonical}");
            Check(WorldCode.LabelForm(v["code"]!.GetValue<string>()) == v["label"]!.GetValue<string>(), $"vector '{name}': label form");
            Check(result.Seed == v["seed"]!.GetValue<uint>(), $"vector '{name}': seed");
            var options = v["options"]!.AsObject();
            Check(options.Count == result.Options!.Count, $"vector '{name}': option count {result.Options.Count}");
            foreach (var (key, value) in options)
                Check(result.Options.TryGetValue(key, out var mine) && SameOption(mine, value), $"vector '{name}': option {key}");
            sample ??= result.Canonical;
            if (sample != result.Canonical) nocturneSample ??= result.Canonical;
        }
        Check(sample != null && nocturneSample != null, "the vectors hold fewer than two valid codes");
        Check(WorldCode.Decode(null).Kind == WorldCodeKind.Bad && WorldCode.Decode("R2-").Kind == WorldCodeKind.Bad && WorldCode.Decode("R2-../..").Kind == WorldCodeKind.Bad, "junk is bad");

        // The generator's version numbers are the launcher's (a fix raises GENERATOR_MAJOR; the label's rando<N> follows).
        var seedcode = File.ReadAllText(RepoFile(Path.Combine("tools", "randomizer", "seedcode.py")));
        Check(Regex.Match(seedcode, @"^GENERATOR_MAJOR = (\d+)", RegexOptions.Multiline).Groups[1].Value == Randomizer.GeneratorMajor.ToString(), "GeneratorMajor differs from seedcode.py");
        Check(Regex.Match(seedcode, @"^SCHEMA = (\d+)", RegexOptions.Multiline).Groups[1].Value == WorldCode.Schema.ToString(), "Schema differs from seedcode.py");

        // ---- label parts
        var code = sample!.ToLowerInvariant();
        var g = Randomizer.GeneratorMajor;
        var label = $"rando{g}:{code}+w:1b8e536a";
        var nlabel = "nocturne@1.0+" + label;
        Check(CuratedMods.WorldOf(label, out var bad) is { Check: "1b8e536a", Nocturne: null } w1 && w1.Generator == g && w1.Code == code && !bad, "a world label");
        Check(CuratedMods.WorldOf(nlabel, out bad) is { Nocturne: "1.0" } && !bad, "a world label on Nocturne");
        Check(CuratedMods.WorldOf("nocturne@1.0", out bad) == null && !bad, "Nocturne alone is no world");
        // Base mods by content (user, 5 Nov): "m:" + 8 hex per mod, in load order, beside at most one Nocturne version.
        var mlabel = "m:0a1b2c3d+" + label;
        Check(CuratedMods.WorldOf(label, out bad) is { Base: null }, "a world on the game alone has no base part");
        Check(CuratedMods.WorldOf(nlabel, out bad) is { Base: "nocturne@1.0" }, "a Nocturne world's base part");
        Check(CuratedMods.WorldOf(mlabel, out bad) is { Base: "m:0a1b2c3d", Nocturne: null } && !bad, "a world on a mod named by content");
        Check(CuratedMods.WorldOf("nocturne@1.0+m:0a1b2c3d+m:ffff0000+" + label, out bad) is { Base: "nocturne@1.0+m:0a1b2c3d+m:ffff0000", Nocturne: "1.0" } && !bad,
            "a world on Nocturne and two mods keeps their order");
        foreach (var malformed in new[] { label + "+w:00000000", label + $"+rando{g}:" + code, label + "+mods:0a1b2c3d", $"rando{g}:{code}", $"rando{g}:{code}+w:1B8E536A",
                                         $"rando{g}:{code}+w:1b8e53", $"randox:{code}+w:1b8e536a", "nocturne@1.0+nocturne@1.1+" + label,
                                         "m:0A1B2C3D+" + label, "m:0a1b2c+" + label, "m:0a1b2c3d0+" + label, "m:0a1b2c3d+m:0a1b2c3d+" + label, "x:0a1b2c3d+" + label,
                                         string.Join("+", Enumerable.Range(0, 9).Select(i => $"m:0000000{i}")) + "+" + label })
            Check(CuratedMods.WorldOf(malformed, out bad) == null && bad, "malformed world label read: " + malformed);

        // ---- the generator build ("g:" + 8 hex, inside w: too): read, and only used to name a mismatch
        var own = "0123abcd" + new string('e', 56);
        var glabel = label + "+g:0123abcd";
        Check(CuratedMods.WorldOf(glabel, out bad) is { Check: "1b8e536a" } && !bad && CuratedMods.WorldOf("nocturne@1.0+" + glabel, out bad) is { Nocturne: "1.0" } && !bad,
            "a world label with its generator build");
        Check(CuratedMods.BuildOf(glabel) == "0123abcd" && CuratedMods.BuildOf(label) == null && CuratedMods.BuildOf(null) == null, "the label's build");
        foreach (var malformed in new[] { glabel + "+g:0123abcd", label + "+g:0123ABCD", label + "+g:0123abc", label + "+g:0123abcde", label + "+g:" })
            Check(CuratedMods.WorldOf(malformed, out bad) == null && bad, "malformed build part read: " + malformed);
        Check(CuratedMods.JoinAdvice(glabel, new LocalMods("my", null, 0, CanBuildWorlds: true), null) == new JoinChoice(JoinKind.BuildWorld, null, sample, "1b8e536a"),
            "a world label with a build is built like one without");
        Check(!CuratedMods.OtherBuild(glabel, own) && CuratedMods.OtherBuild(glabel, "0123abce" + new string('e', 56)), "another build is another build");
        Check(CuratedMods.OtherBuild(label, own), "a host label without a build: a generator before build ids, so another build");
        Check(!CuratedMods.OtherBuild(null, own) && !CuratedMods.OtherBuild(glabel, null), "no label or an unknown build: nothing to tell");

        // ---- join advice
        var plain = new LocalMods("my", null, 0, null, null, null, true);
        var choice = CuratedMods.JoinAdvice(label, plain, new[] { "1.0" });
        Check(choice == new JoinChoice(JoinKind.BuildWorld, null, sample, "1b8e536a"), "a world on the game alone is built: " + choice);
        Check(CuratedMods.JoinAdvice(nlabel, plain, new[] { "1.0" }) == new JoinChoice(JoinKind.BuildWorld, "1.0", sample, "1b8e536a", Base: "nocturne@1.0"), "a world on an offered Nocturne is built");
        Check(CuratedMods.JoinAdvice(nlabel, new LocalMods("nocturne", "1.0", 0, null, null, null, true), null).Kind == JoinKind.BuildWorld, "an installed Nocturne needs no manifest");
        Check(CuratedMods.JoinAdvice(nlabel, plain, new[] { "1.1" }) == new JoinChoice(JoinKind.NocturneNotAvailable, "1.0"), "a Nocturne not offered is not available");
        Check(CuratedMods.JoinAdvice(nlabel, new LocalMods("world", "1.0", 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: true, WorldGenerator: g,
            WorldBase: "nocturne@1.0|m:12345678"), null).Kind == JoinKind.Join, "the same world plays: join");
        Check(CuratedMods.JoinAdvice(nlabel, new LocalMods("world", "1.0", 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: true, WorldGenerator: g,
            WorldBase: "m:12345678"), null).Kind == JoinKind.BuildWorld, "the same world on another base here is no match (the party hash covers the base)");
        Check(CuratedMods.JoinAdvice(label, new LocalMods("world", null, 0, World: $"Randomizer {sample} 00000000", CanBuildWorlds: true, WorldGenerator: g), null).Kind == JoinKind.BuildWorld, "another build of the code is no match");
        Check(CuratedMods.JoinAdvice(label, plain with { CanBuildWorlds = false }, null).Kind == JoinKind.WorldNotAvailable, "a package without the generator can't build");
        Check(CuratedMods.JoinAdvice($"rando{g + 1}:{code}+w:1b8e536a", plain, null).Kind == JoinKind.WorldNewer, "a newer generator: update");
        Check(CuratedMods.JoinAdvice($"rando{g + 1}:{code}+w:1b8e536a", new LocalMods("world", null, 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: true, WorldGenerator: g), null).Kind == JoinKind.WorldNewer,
            "a newer generator's world is not the one of that name here");
        Check(CuratedMods.JoinAdvice($"rando{g}:r3-00000-00+w:1b8e536a", plain, null).Kind == JoinKind.WorldNewer, "a newer code schema: update");
        Check(CuratedMods.JoinAdvice($"rando{g}:r2-00000-00+w:1b8e536a", plain, null).Kind == JoinKind.Modded, "a bad code only shows modded");
        if (code.Contains('0'))
            Check(CuratedMods.JoinAdvice($"rando{g}:{code.Replace('0', 'o')}+w:1b8e536a", plain, null).Kind == JoinKind.Modded, "a label code must be canonical (no look-alikes)");

        // An older generator's world (label rando<g - 1>): the host must make it again; nothing is built, and a
        // folder of that name here joins only when it is that generator's world too (the name holds no generator).
        var oldLabel = $"rando{g - 1}:{code}+w:1b8e536a";
        Check(CuratedMods.JoinAdvice(oldLabel, plain, null) == new JoinChoice(JoinKind.WorldOlder, null, sample, "1b8e536a"), "an older generator's world: the host makes it again");
        Check(CuratedMods.JoinAdvice("nocturne@1.0+" + oldLabel, plain, new[] { "1.0" }).Kind == JoinKind.WorldOlder, "an older world on Nocturne: older, nothing fetched");
        Check(CuratedMods.JoinAdvice($"rando0:{code}+w:1b8e536a", plain, null).Kind == JoinKind.WorldOlder, "any older generator is older");
        Check(CuratedMods.JoinAdvice(oldLabel, new LocalMods("world", null, 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: true, WorldGenerator: g), null).Kind == JoinKind.WorldOlder,
            "this generator's world of the same name is not the host's older world");
        Check(CuratedMods.JoinAdvice(oldLabel, new LocalMods("world", null, 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: true, WorldGenerator: g - 1), null).Kind == JoinKind.Join,
            "a guest already playing that older world joins it");
        Check(CuratedMods.JoinAdvice(label, new LocalMods("world", null, 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: true, WorldGenerator: g - 1), null) == new JoinChoice(JoinKind.BuildWorld, null, sample, "1b8e536a"),
            "an older world of the host's name here is no match: built again");
        Check(CuratedMods.JoinAdvice(label, new LocalMods("world", null, 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: true, WorldGenerator: null), null).Kind == JoinKind.BuildWorld,
            "a world of that name with an unreadable manifest is no match");
        Check(CuratedMods.JoinAdvice(label, new LocalMods("world", null, 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: false, WorldGenerator: g - 1), null).Kind == JoinKind.WorldNotAvailable,
            "an older world of that name and no generator: not available");

        // The happy path, like "Use Nocturne X and join": a guest without the world who can build it gets "Build world and
        // join", also when the host's Nocturne is missing here but offered (UseWorldAsync fetches it first).
        Check(CuratedMods.JoinAdvice(label, new LocalMods("standard", null, 0, World: null, CanBuildWorlds: true), null) == new JoinChoice(JoinKind.BuildWorld, null, sample, "1b8e536a"),
            "a vanilla guest builds the world");
        Check(CuratedMods.JoinAdvice(nlabel, new LocalMods("nocturne", "0.9", 0, World: null, CanBuildWorlds: true), new[] { "0.9", "1.0" }) == new JoinChoice(JoinKind.BuildWorld, "1.0", sample, "1b8e536a", Base: "nocturne@1.0"),
            "another Nocturne here, the host's offered: build (Nocturne first)");
        Check(CuratedMods.JoinAdvice("nocturne@1.0", new LocalMods("my", null, 0, World: null, CanBuildWorlds: true), new[] { "1.0" }) == new JoinChoice(JoinKind.GetNocturne, "1.0"),
            "Nocturne alone, missing and offered: get it (the same rule)");

        Check(CuratedMods.JoinAdvice(label + "+mods:0a1b2c3d", plain, null).Kind == JoinKind.Modded, "a malformed world label only shows modded");

        // 0.2.8 hosts add "+patches:" + 8 hex after the world label (party_vpnlobby.h party_label): the world part is read
        // without it, the patches compared on their own.
        Check(CuratedMods.JoinAdvice(nlabel + "+patches:d8e32e8a", plain, new[] { "1.0" }) == new JoinChoice(JoinKind.BuildWorld, "1.0", sample, "1b8e536a", Base: "nocturne@1.0"),
            "a world label with a patches part is built");
        Check(CuratedMods.JoinAdvice(label + "+patches:00000001", plain with { PatchesHash = "d8e32e8a" }, null) is { Kind: JoinKind.BuildWorld, PatchesDiffer: true },
            "other patches on a world host are told");
        Check(CuratedMods.WorldOf(label + "+patches:00000001", out bad) == null && bad && CuratedMods.WorldOf(CuratedMods.ModsPart(label + "+patches:00000001"), out bad) != null,
            "WorldOf reads the mods part only");

        // A mod named by content: found here under any folder name (BaseTokens), else named as missing; unknown yet: built,
        // UseWorldAsync looks.
        Check(CuratedMods.JoinAdvice(mlabel, plain with { BaseTokens = "m:0a1b2c3d,m:ffff0000" }, null) == new JoinChoice(JoinKind.BuildWorld, null, sample, "1b8e536a", Base: "m:0a1b2c3d"),
            "a world on a mod this PC has (any folder name) is built");
        Check(CuratedMods.JoinAdvice(mlabel, plain with { BaseTokens = "m:ffff0000" }, null) == new JoinChoice(JoinKind.WorldModMissing, null, sample, "1b8e536a", Base: "m:0a1b2c3d", Missing: "m:0a1b2c3d"),
            "a world on a mod this PC lacks says which (by content)");
        Check(CuratedMods.JoinAdvice(mlabel, plain, null).Kind == JoinKind.BuildWorld, "mods not worked out yet: build (UseWorldAsync looks)");
        Check(CuratedMods.MissingModsText(new[] { "m:0a1b2c3d", "nocturne@1.1" }) is var text && text.Contains("m:0a1b2c3d") && text.Contains("Nocturne 1.1"),
            "the missing-mods message names the keys");
        Check(CuratedMods.JoinAdvice(nlabel, plain with { BaseTokens = "m:12121212,nocturne@1.0" }, null) == new JoinChoice(JoinKind.BuildWorld, "1.0", sample, "1b8e536a", Base: "nocturne@1.0"),
            "a renamed Nocturne of the host's version is used (no manifest needed)");
        Check(CuratedMods.JoinAdvice(nlabel, plain with { BaseTokens = "m:12121212,nocturne@0.9" }, null).Kind == JoinKind.NocturneNotAvailable,
            "another Nocturne version here and none offered: not available");
        Check(CuratedMods.JoinAdvice(mlabel, new LocalMods("world", null, 0, World: $"Randomizer {sample} 1b8e536a", CanBuildWorlds: true, WorldGenerator: g,
            WorldBase: "nocturne@1.0|m:0a1b2c3d"), null).Kind == JoinKind.Join, "a host's Nocturne copy named by content matches a Nocturne known by version here");

        // ---- world mode: mods.json holds the world's base and the world, My mods comes back after
        var tmp = Path.Combine(Path.GetTempPath(), "nw-world-" + Guid.NewGuid().ToString("N"));
        var before = Environment.GetEnvironmentVariable("BB_DATA_DIR");
        try
        {
            var data = Path.Combine(tmp, "data");
            var mods = Path.Combine(data, "mods");
            Environment.SetEnvironmentVariable("BB_DATA_DIR", data);
            void Put(string path, string text) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, text); }
            foreach (var n in new[] { "ModA", "Nocturne v1.0" }) Put(Path.Combine(mods, n, "dvdroot_ps4", "param", n + ".bin"), n);
            var nocturneHash = CuratedMods.ContentHash(Path.Combine(mods, "Nocturne v1.0"));
            Put(Path.Combine(mods, "Nocturne v1.0", "nocturne.json"), "{\"version\":\"1.0\",\"content\":\"" + nocturneHash + "\"}");
            var hash = new string('a', 8) + new string('0', 56);
            string World(string c, string h, IEnumerable<string> baseMods, string? baseLabel, int? generator = null, string? build = null)
            {
                var folder = $"Randomizer {c} {h[..8]}";
                var lobby = (baseLabel != null ? baseLabel + "+" : "") + $"rando{generator ?? g}:{c.ToLowerInvariant()}+w:{h[..8]}" +
                            (build != null ? "+g:" + build[..8] : "");
                var manifest = new JsonObject
                {
                    ["generator"] = generator ?? g, ["code"] = c, ["input_hash"] = h, ["mods"] = new JsonArray(baseMods.Select(n => (JsonNode?)n).ToArray()),
                    ["base_label"] = baseLabel, ["lobby_label"] = lobby,
                };
                if (build != null) manifest["generator_build"] = build;
                Put(Path.Combine(mods, folder, "randomizer_manifest.json"), manifest.ToJsonString());
                Put(Path.Combine(mods, folder, "dvdroot_ps4", "map", "x.bin"), "x");
                return folder;
            }
            var onNocturne = World(sample, hash, new[] { "Nocturne v1.0" }, "nocturne@1.0");
            var alone = World(nocturneSample!, hash, Array.Empty<string>(), null);
            var onModA = World(sample, "bbbbbbbb" + new string('0', 56), new[] { "ModA" }, null);
            Mods.Save(new[] { new Mods.Entry("ModA", true), new Mods.Entry("Nocturne v1.0", false), new Mods.Entry(onNocturne, false), new Mods.Entry(alone, false), new Mods.Entry(onModA, false) });

            var s = new LauncherSettings();
            s.Set("mods_enabled", true);  // My mods (0.2.7: the Load mods dropdown; mods_enabled off reads as No mods)
            Randomizer.Use(s, onNocturne);
            Check(CuratedMods.Mode(s) == "world" && CuratedMods.ModsOn(s), "Use picks world mode");
            CuratedMods.Apply(s, mods);
            Check(Mods.Load(mods).Where(m => m.Enabled).Select(m => m.Name).SequenceEqual(new[] { "Nocturne v1.0", onNocturne }), "world mode loads its base, then the world");
            Check(Randomizer.Active(s)?.Folder == onNocturne, "the active world is world mode's");
            Check(CuratedMods.HostLabel(s, mods) == $"nocturne@1.0+rando{g}:{code}+w:aaaaaaaa", "a world on verified Nocturne announces its lobby label");
            Check(CuratedMods.Local(s, mods, null) is { Mode: "world", World: var lw, WorldGenerator: var lg } && lw == onNocturne && lg == g, "the guest side knows its world and its generator");

            Randomizer.Use(s, alone);
            CuratedMods.Apply(s, mods);
            Check(Mods.Load(mods).Where(m => m.Enabled).Select(m => m.Name).SequenceEqual(new[] { alone }), "a world on the game alone loads alone");
            Check(CuratedMods.HostLabel(s, mods) == $"rando{g}:{nocturneSample!.ToLowerInvariant()}+w:aaaaaaaa", "a world on the game alone has no base part");

            Randomizer.Use(s, onModA);
            CuratedMods.Apply(s, mods);
            Check(CuratedMods.HostLabel(s, mods) == null, "a world built before base mods by content (no base label) announces no world label");

            Randomizer.StopUsing(s);
            CuratedMods.Apply(s, mods);
            var back = Mods.Load(mods).ToDictionary(m => m.Name, m => m.Enabled);
            Check(CuratedMods.Mode(s) == "my" && back["ModA"] && !back["Nocturne v1.0"] && !back[onNocturne], "leaving world mode brings My mods back");

            // A changed Nocturne, a hand-edited manifest or a missing world: no world label, and a missing world leaves the mode.
            Randomizer.Use(s, onNocturne);
            CuratedMods.Apply(s, mods);
            Put(Path.Combine(mods, "Nocturne v1.0", "dvdroot_ps4", "param", "extra.bin"), "changed");
            Check(CuratedMods.HostLabel(s, mods) == null, "a changed Nocturne announces no world label");
            File.Delete(Path.Combine(mods, "Nocturne v1.0", "dvdroot_ps4", "param", "extra.bin"));
            var manifestPath = Path.Combine(mods, onNocturne, "randomizer_manifest.json");
            var edited = JsonNode.Parse(File.ReadAllText(manifestPath))!;
            edited["lobby_label"] = $"nocturne@1.0+rando{g}:" + code + "+w:00000000";
            File.WriteAllText(manifestPath, edited.ToJsonString());
            Check(CuratedMods.HostLabel(s, mods) == null, "a manifest whose label disagrees with its folder announces nothing");
            Directory.Delete(Path.Combine(mods, onNocturne), true);
            CuratedMods.Apply(s, mods);
            Check(CuratedMods.Mode(s) == "my" && Randomizer.Active(s) == null, "a deleted world leaves world mode");

            // An older generator's world: it still plays (world mode loads it), announces no world label (the game then
            // announces "mods:" + its hash), and the join flow builds the host's world again instead of reusing it.
            var oldHash = "cccccccc" + new string('0', 56);
            var old = World(sample, oldHash, Array.Empty<string>(), null, g - 1);
            Randomizer.Use(s, old);
            CuratedMods.Apply(s, mods);
            Check(Mods.Load(mods).Where(m => m.Enabled).Select(m => m.Name).SequenceEqual(new[] { old }), "an older world still plays as built");
            Check(CuratedMods.HostLabel(s, mods) == null, "an older world announces no world label");
            Check(CuratedMods.Local(s, mods, null) is { World: var ow, WorldGenerator: var og } && ow == old && og == g - 1, "the guest side knows the older world's generator");
            Check(CuratedMods.JoinAdvice($"rando{g}:{code}+w:cccccccc", CuratedMods.Local(s, mods, null) with { CanBuildWorlds = true }, null).Kind == JoinKind.BuildWorld,
                "an older world of the host's name: built again, no join");
            Check(!CuratedMods.HasWorld(mods, sample, "cccccccc", null), "an older world of the name is not this generator's world");
            var startedBuild = false;
            try
            {
                CuratedMods.UseWorldAsync(s, mods, new JoinChoice(JoinKind.BuildWorld, null, sample, "cccccccc"), null, null,
                    () => { startedBuild = true; throw new OperationCanceledException("test: no generator here"); }).GetAwaiter().GetResult();
            }
            catch (OperationCanceledException)
            {
            }
            Check(startedBuild && Directory.Exists(Path.Combine(mods, old)), "UseWorldAsync builds again over an older world of that name (and leaves it until the build is done)");

            var current = World(sample, "dddddddd" + new string('0', 56), Array.Empty<string>(), null);
            Check(CuratedMods.HasWorld(mods, sample, "dddddddd", null), "this generator's world of the name is reused");
            Check(!CuratedMods.HasWorld(mods, sample, "dddddddd", "nocturne@1.0"), "on another base it is not");
            Check(!CuratedMods.HasWorld(mods, nocturneSample!, "dddddddd", null), "another code is not");
            CuratedMods.UseWorldAsync(s, mods, new JoinChoice(JoinKind.BuildWorld, null, sample, "dddddddd"), null, null,
                () => throw new InvalidOperationException("test: a world in place must not be built")).GetAwaiter().GetResult();
            Check(CuratedMods.Mode(s) == "world" && s.Str(Randomizer.WorldKey) == current, "UseWorldAsync plays the world in place");

            // Another generator build (R3-REV, 5 Nov): one code and the same files give another world, under another w: (the
            // build is in the input hash), so a local copy of another build never has the host's folder name and is built
            // again; when this package's build is not the label's g:, the guest is told so before any build.
            var withBuild = World(sample, "eeeeeeee" + new string('0', 56), Array.Empty<string>(), null, build: own);
            Check(Randomizer.ReadManifest(Path.Combine(mods, withBuild))?.GeneratorBuild == own, "the manifest's generator build is read");
            Randomizer.Use(s, withBuild);
            CuratedMods.Apply(s, mods);
            Check(CuratedMods.HostLabel(s, mods) == $"rando{g}:{code}+w:eeeeeeee+g:0123abcd", "a world with a generator build announces it");
            Check(!CuratedMods.HasWorld(mods, sample, "ffffffff", null), "a local copy of another build (another w:) is not the host's world");
            var hostOther = $"rando{g}:{code}+w:ffffffff+g:99999999";
            var savedBuild = Randomizer.KnownBuildId;
            try
            {
                Randomizer.KnownBuildId = own;
                var built = false;
                Exception? told = null;
                try
                {
                    CuratedMods.UseWorldAsync(s, mods, new JoinChoice(JoinKind.BuildWorld, null, sample, "ffffffff"), null, null,
                        () => built = true, hostLabel: hostOther).GetAwaiter().GetResult();
                }
                catch (InvalidDataException e)
                {
                    told = e;
                }
                Check(!built && told?.Message == CuratedMods.OtherBuildText, "another host build is refused before the build, with the build message: " + told?.Message);
                built = false;
                told = null;
                try
                {
                    CuratedMods.UseWorldAsync(s, mods, new JoinChoice(JoinKind.BuildWorld, null, sample, "ffffffff"), null, null,
                        () => { built = true; throw new OperationCanceledException("test: no generator here"); }, hostLabel: $"rando{g}:{code}+w:ffffffff+g:0123abcd").GetAwaiter().GetResult();
                }
                catch (OperationCanceledException)
                {
                }
                Check(built, "the host's own build goes on to build");
                CuratedMods.UseWorldAsync(s, mods, new JoinChoice(JoinKind.Join, null, sample, "eeeeeeee"), null, null,
                    () => throw new InvalidOperationException("test: a world in place must not be built"), hostLabel: hostOther).GetAwaiter().GetResult();
                Check(s.Str(Randomizer.WorldKey) == withBuild, "a world already here is played whatever build the label names (it is the host's by name and hash)");
            }
            finally
            {
                Randomizer.KnownBuildId = savedBuild;
            }
            Randomizer.StopUsing(s);
            CuratedMods.Apply(s, mods);

            // ---- base mods by content: the same files under another folder name are the same base
            foreach (var n in new[] { "ModA copy", "My Nocturne" })
            {
                var from = Path.Combine(mods, n == "ModA copy" ? "ModA" : "Nocturne v1.0");
                foreach (var f in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
                    Put(Path.Combine(mods, n, Path.GetRelativePath(from, f)), File.ReadAllText(f));
            }
            var a8 = CuratedMods.LoadedHash(Path.Combine(mods, "ModA"))[..8];
            Check(CuratedMods.LoadedHash(Path.Combine(mods, "ModA copy"))[..8] == a8, "a copy under another name has the same content hash");
            Check(Randomizer.BaseLabel(mods, new[] { "ModA" }) == (true, "m:" + a8) && Randomizer.BaseLabel(mods, new[] { "ModA copy" }) == (true, "m:" + a8),
                "a mod is named by content, not folder name");
            Check(Randomizer.BaseLabel(mods, new[] { "My Nocturne" }) == (true, "nocturne@1.0"), "a renamed Nocturne is known by its marker and content");
            Check(Randomizer.BaseLabel(mods, new[] { "My Nocturne", "ModA copy" }) == (true, "nocturne@1.0+m:" + a8), "base keys keep the load order");
            Check(Randomizer.BaseLabel(mods, new[] { "ModA", "ModA copy" }) == (false, null), "the same files twice: not shareable");
            Check(Randomizer.BaseLabel(mods, new[] { "Gone" }) == (false, null), "a missing base folder: not shareable");
            Check(CuratedMods.ResolveBase(mods, "m:" + a8) is { Folders: [var r1] } && (r1 == "ModA" || r1 == "ModA copy"), "a mod by content is found under any name");
            Check(CuratedMods.ResolveBase(mods, "nocturne@1.0+m:deadbeef") is { Folders: null, Missing: ["m:deadbeef"] }, "a mod no folder has is named as missing");
            Check(CuratedMods.ResolveBase(mods, null) is { Folders: [] }, "the game alone needs no folder");
            var copyWorld = World(sample, "eeeeeeee" + new string('0', 56), new[] { "ModA copy" }, "m:" + a8);
            Check(CuratedMods.HasWorld(mods, sample, "eeeeeeee", "m:" + a8), "a world built on the copy is the host's world on the original");
            Randomizer.Use(s, copyWorld);
            CuratedMods.Apply(s, mods);
            Check(CuratedMods.HostLabel(s, mods) == $"m:{a8}+rando{g}:{code}+w:eeeeeeee", "a world on another mod announces it by content");
            Check(CuratedMods.JoinAdvice($"m:{a8}+rando{g}:{code}+w:eeeeeeee", CuratedMods.Local(s, mods, null), null).Kind == JoinKind.Join,
                "a guest playing the same world on the same files under another name joins");
            Randomizer.StopUsing(s);
            CuratedMods.Apply(s, mods);
            CuratedMods.UseWorldAsync(s, mods, new JoinChoice(JoinKind.BuildWorld, null, sample, "eeeeeeee", Base: "m:" + a8), null, null,
                () => throw new InvalidOperationException("test: a world in place must not be built")).GetAwaiter().GetResult();
            Check(s.Str(Randomizer.WorldKey) == copyWorld && Mods.Load(mods).Where(m => m.Enabled).Select(m => m.Name).SequenceEqual(new[] { "ModA copy", copyWorld }),
                "UseWorldAsync plays a world in place built on the same files under another name");
            Randomizer.StopUsing(s);
            CuratedMods.Apply(s, mods);
            var missingSaid = "";
            try
            {
                CuratedMods.UseWorldAsync(s, mods, new JoinChoice(JoinKind.BuildWorld, null, sample, "ffffffff", Base: "m:deadbeef"), null, null,
                    () => throw new InvalidOperationException("test: no build without the base")).GetAwaiter().GetResult();
            }
            catch (InvalidDataException e)
            {
                missingSaid = e.Message;
            }
            Check(missingSaid.Contains("m:deadbeef"), "UseWorldAsync names a missing mod by content and builds nothing: " + missingSaid);
            var tokens = "";
            for (var i = 0; i < 200 && (tokens = CuratedMods.KnownBaseTokens(mods) ?? "") == ""; i++) Thread.Sleep(50);
            Check(tokens.Split(',').Contains("m:" + a8) && tokens.Split(',').Contains("nocturne@1.0"), "this PC's base keys are worked out: " + tokens);

            Check(Randomizer.GenerateArguments("g", "c", "m", new[] { "a" }, "nocturne@1.0").SequenceEqual(new[] { "generate", "--game", "g", "--code", "c", "--out", "m", "--base-label", "nocturne@1.0", "--mods", "a" }),
                "the base label goes to the generator");
            Check(Randomizer.BaseLabel(mods, Array.Empty<string>()) == (true, null) &&
                  Randomizer.BaseLabel(mods, new[] { "Nocturne v1.0" }) == (true, "nocturne@1.0"), "base labels");
        }
        finally
        {
            Environment.SetEnvironmentVariable("BB_DATA_DIR", before);
            try { Directory.Delete(tmp, true); } catch (IOException) { }
        }
    }
}
