using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Updates from the GitHub releases of the repository. Each release carries the whole package
    /// (LanternEngine-*.zip; BloodbornePC-*.zip before beta 9.6), the launcher on its own ("Bloodborne Launcher.exe") and package.json,
    /// which names the engine build inside the package.
    ///
    /// When only the launcher changed, the exe alone is downloaded. When the engine changed (or the
    /// package has no package.json yet), the whole package is downloaded and copied over this one,
    /// keeping the player's settings, saves, mods and profiles. A running exe cannot be overwritten
    /// but can be renamed, so the launcher moves itself aside and is removed on the next start.
    ///
    /// Two channels: stable follows GitHub's latest release; beta also takes pre-releases, tagged
    /// v&lt;version&gt;-beta.&lt;n&gt;. A beta comes before the stable release of the same version.
    /// </summary>
    public static class Updater
    {
        public const string Repository = "vladtepes22/NightwatchCoop";
        public static string ReleasesPage => $"https://github.com/{Repository}/releases";

        /// <summary>The engine build this launcher is made for (package.json "engine"): one per release
        /// ("nightwatch-coop-0.2.0-beta.8"), so every update replaces the co-op engine with the launcher.</summary>
        public static string EngineId { get; } = "nightwatch-coop-" + ReleaseVersion();

        internal static string ReleaseVersion()
        {
            var text = typeof(Updater).Assembly.GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
                .OfType<System.Reflection.AssemblyInformationalVersionAttribute>().FirstOrDefault()?.InformationalVersion ?? "";
            var plus = text.IndexOf('+');
            return plus > 0 ? text[..plus] : text.Length > 0 ? text : Current.ToString();
        }

        public const string ExeName = "Nightwatch Co-op.exe";
        /// <summary>The launcher's names before v0.2.0 beta 8 ("Lantern Engine.exe", now a forwarder to
        /// ExeName) and before Lantern 0.9.34 beta 8.</summary>
        public static readonly string[] OldExeNames = { "Lantern Engine.exe", "Bloodborne Launcher.exe" };
        private const string PackageFile = "package.json";

        /// <summary>Files and folders of the player's that a package update never touches.</summary>
        private static readonly string[] Kept = { "bbport.ini", "mods.json", "patches.json", "user", "mods", "logs", "network", "engine-assets" };

        public sealed record Release(Version Version, int Beta, string Tag, string Name, string Notes, string Page,
            string? ExeUrl, string? ZipUrl, string? PackageUrl, long ExeSize, long ZipSize, DateTime Published = default)
        {
            public bool IsNewerThan(Version version, int beta) => Compare(Version, Beta, version, beta) > 0;
            public bool IsCurrent => Compare(Version, Beta, Current, CurrentBeta) == 0;
            /// <summary>A release published with only its zip (no launcher exe, no package.json): it is
            /// installed by hand into a new folder, not in place (beta 9.2 was the first).</summary>
            public bool FreshDownload => ZipUrl != null && ExeUrl == null && PackageUrl == null;
        }

        public static Version Current
        {
            get
            {
                var v = typeof(Updater).Assembly.GetName().Version ?? new Version(0, 0, 0);
                return new Version(v.Major, v.Minor, Math.Max(0, v.Build));
            }
        }

        /// <summary>The beta number of this build (from its informational version, "0.9.7-beta.2"), 0 when stable.</summary>
        public static int CurrentBeta { get; } =
            TryVersion(typeof(Updater).Assembly.GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
                .OfType<System.Reflection.AssemblyInformationalVersionAttribute>().FirstOrDefault()?.InformationalVersion ?? "", out _, out var beta)
                ? beta
                : 0;

        /// <summary>This build as players see it: "v0.9.6" or "v0.9.7 beta 2".</summary>
        public static string CurrentLabel => Label(Current, CurrentBeta);

        /// <summary>The channel the player chose; until they choose, a beta build stays on beta.</summary>
        public static bool BetaChannel(LauncherSettings s) => s.Str("update_channel") switch
        {
            "beta" => true,
            "stable" => false,
            _ => CurrentBeta > 0,
        };

        public static string Label(Version version, int beta) =>
            beta <= 0 ? $"v{version}"
            : beta % 100 == 0 ? $"v{version} beta {beta / 100}" : $"v{version} beta {beta / 100}.{beta % 100}";

        /// <summary>Orders builds: by version, and a version's betas before its stable release.</summary>
        public static int Compare(Version a, int betaA, Version b, int betaB)
        {
            var byVersion = a.CompareTo(b);
            if (byVersion != 0) return byVersion;
            return (betaA == 0 ? int.MaxValue : betaA).CompareTo(betaB == 0 ? int.MaxValue : betaB);
        }

        /// <summary>The package this launcher runs in (it ships inside it), or null when it runs on its own.</summary>
        public static string? OwnPackage =>
            Paths.DetectPortDir() is { } dir && Environment.ProcessPath is { } self &&
            string.Equals(Path.GetDirectoryName(self)?.TrimEnd('\\'), dir.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)
                ? dir
                : null;

        /// <summary>The engine build in the package, from its package.json; null when it has none.</summary>
        public static string? LocalEngine()
        {
            try
            {
                var file = Path.Combine(Paths.PortDir, PackageFile);
                return File.Exists(file) && JsonNode.Parse(File.ReadAllText(file)) is JsonObject o ? (string?)o["engine"] : null;
            }
            catch
            {
                return null;
            }
        }

        /// <summary>The package needs the engine this launcher is made for.</summary>
        public static bool EngineOutdated => OwnPackage != null && LocalEngine() != EngineId;

        /// <summary>The latest release on the channel, or null when there is none (or no network).
        /// Stable: GitHub's latest release. Beta: the newest of all releases, pre-releases included.</summary>
        public static async Task<Release?> LatestAsync(bool beta = false)
        {
            using var http = Client(TimeSpan.FromSeconds(10));
            using var response = await http.GetAsync(beta
                ? $"https://api.github.com/repos/{Repository}/releases?per_page=30"
                : $"https://api.github.com/repos/{Repository}/releases/latest");
            if (response.StatusCode == System.Net.HttpStatusCode.NotFound)
                return null;
            response.EnsureSuccessStatusCode();
            using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            if (!beta)
                return Parse(doc.RootElement);

            Release? newest = null;
            foreach (var item in doc.RootElement.EnumerateArray())
            {
                if (item.TryGetProperty("draft", out var draft) && draft.GetBoolean()) continue;
                if (Parse(item) is { } release && (newest == null || release.IsNewerThan(newest.Version, newest.Beta)))
                    newest = release;
            }
            return newest;
        }

        /// <summary>Every published release, stable and beta, newest first (the version selector).</summary>
        public static async Task<IReadOnlyList<Release>> AllAsync()
        {
            using var http = Client(TimeSpan.FromSeconds(15));
            using var response = await http.GetAsync($"https://api.github.com/repos/{Repository}/releases?per_page=100");
            response.EnsureSuccessStatusCode();
            using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            var all = new List<Release>();
            foreach (var item in doc.RootElement.EnumerateArray())
            {
                if (item.TryGetProperty("draft", out var draft) && draft.GetBoolean()) continue;
                if (Parse(item) is { } release && (release.ZipUrl != null || release.ExeUrl != null))
                    all.Add(release);
            }
            all.Sort((a, b) => Compare(b.Version, b.Beta, a.Version, a.Beta));
            return all;
        }

        /// <summary>Whether switching to a chosen release replaces the whole package: its engine differs
        /// from this one's, or it does not say which engine it has (older releases).</summary>
        public static async Task<bool> SwitchNeedsPackageAsync(Release release)
        {
            if (OwnPackage == null || release.ZipUrl == null) return false;
            if (release.ExeUrl == null) return true;
            var remote = await RemoteEngineAsync(release);
            return remote == null || remote != LocalEngine();
        }

        private static Release? Parse(JsonElement root)
        {
            var tag = root.GetProperty("tag_name").GetString() ?? "";
            if (!TryVersion(tag, out var version, out var betaNumber))
                return null;

            string? exe = null, zip = null, package = null;
            long exeSize = 0, zipSize = 0;
            foreach (var asset in root.GetProperty("assets").EnumerateArray())
            {
                var name = asset.GetProperty("name").GetString() ?? "";
                var url = asset.GetProperty("browser_download_url").GetString();
                var size = asset.GetProperty("size").GetInt64();
                // GitHub stores "Nightwatch Co-op.exe" as "Nightwatch.Co-op.exe". A release before the
                // rename has only an old name.
                var spaced = name.Replace(".", " ").Replace(" exe", ".exe");
                if (spaced.Equals(ExeName, StringComparison.OrdinalIgnoreCase) ||
                    (exe == null && OldExeNames.Any(old => spaced.Equals(old, StringComparison.OrdinalIgnoreCase))))
                    (exe, exeSize) = (url, size);
                else if (name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) && zip == null)
                    (zip, zipSize) = (url, size);
                else if (name.Equals(PackageFile, StringComparison.OrdinalIgnoreCase))
                    package = url;
            }
            return new Release(version, betaNumber, tag,
                root.TryGetProperty("name", out var n) && n.GetString() is { Length: > 0 } title ? title : tag,
                root.TryGetProperty("body", out var b) ? b.GetString() ?? "" : "",
                root.GetProperty("html_url").GetString() ?? ReleasesPage, exe, zip, package, exeSize, zipSize,
                root.TryGetProperty("published_at", out var p) && p.ValueKind == JsonValueKind.String &&
                DateTime.TryParse(p.GetString(), null, System.Globalization.DateTimeStyles.RoundtripKind, out var when)
                    ? when.ToLocalTime()
                    : default);
        }

        /// <summary>Reads "v0.9.7", "0.9.7-beta.2" or "0.9.7-beta2+abc123"; <paramref name="beta"/> is 0 for a stable build.</summary>
        public static bool TryVersion(string tag, out Version version, out int beta)
        {
            var text = tag.Trim().TrimStart('v', 'V');
            var plus = text.IndexOf('+');
            if (plus > 0) text = text[..plus];
            beta = 0;
            var marker = text.IndexOf("-beta", StringComparison.OrdinalIgnoreCase);
            if (marker > 0)
            {
                // "beta.9" and "beta.9.1" (a small update to beta 9): kept as 900 and 901, so 9 <
                // 9.1 < 10. Launchers before 9.1 read "9.1" as 91, which is newer too: they update.
                var parts = text[(marker + 5)..].TrimStart('.', '-', ' ').Split('.', '-', ' ', '+');
                var major = parts.Length > 0 && int.TryParse(parts[0], out var a) && a > 0 ? a : 1;
                var minor = parts.Length > 1 && int.TryParse(parts[1], out var b) && b is > 0 and < 100 ? b : 0;
                beta = major * 100 + minor;
            }
            var cut = text.IndexOfAny(new[] { '-', '+', ' ' });
            if (cut > 0) text = text[..cut];
            if (Version.TryParse(text.Contains('.') ? text : text + ".0", out var v))
            {
                version = new Version(v.Major, v.Minor, Math.Max(0, v.Build));
                return true;
            }
            version = new Version(0, 0, 0);
            return false;
        }

        /// <summary>The engine build the release's package has, or null when it does not say. Fetch failures throw.</summary>
        public static async Task<string?> RemoteEngineAsync(Release release)
        {
            if (release.PackageUrl == null) return null;
            try
            {
                using var http = Client(TimeSpan.FromSeconds(10));
                return JsonNode.Parse(await http.GetStringAsync(release.PackageUrl)) is JsonObject o ? (string?)o["engine"] : null;
            }
            catch
            {
                throw new InvalidOperationException("Could not read the release manifest. Check your connection and try the update again.");
            }
        }

        /// <summary>Whether this update has to replace the whole package (a new engine).</summary>
        public static async Task<bool> NeedsPackageAsync(Release release)
        {
            if (OwnPackage == null || release.ZipUrl == null) return false;
            var remote = await RemoteEngineAsync(release);
            return remote != null ? remote != LocalEngine() : LocalEngine() == null && release.ExeUrl == null;
        }

        /// <summary>Downloads the update, puts it in place and starts the new launcher; the caller then exits.
        /// Returns the package files that could not be put in place and were left as they were (only
        /// optional ones: see <see cref="Optional"/>).</summary>
        public static async Task<IReadOnlyList<string>> InstallAsync(Release release, bool package, IProgress<double>? progress = null)
        {
            var self = Environment.ProcessPath ?? throw new InvalidOperationException("Cannot tell where this launcher is.");
            var folder = Path.GetDirectoryName(self)!;
            // A folder of its own each time: one an antivirus still holds a file in (from an earlier
            // attempt) cannot be deleted, and must not stop this one.
            var parent = Path.Combine(Path.GetTempPath(), "bbport-launcher-update");
            if (Directory.Exists(parent))
            {
                foreach (var old in Directory.GetDirectories(parent).Append(parent))
                    try
                    {
                        // An interrupted update or a blocked rollback may still need its originals.
                        if (old != parent && !Directory.EnumerateDirectories(old, "*.backup", SearchOption.AllDirectories)
                                .Any(d => Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories).Any()))
                            Directory.Delete(old, recursive: true);
                    }
                    catch (Exception) { }
                foreach (var old in Directory.GetFiles(parent))
                    try { File.Delete(old); } catch (Exception) { }
            }
            var work = Path.Combine(parent, DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            Directory.CreateDirectory(work);
            var skipped = new List<string>();

            if (package)
            {
                if (Process.GetProcessesByName("bb-probe").Length > 0)
                    throw new InvalidOperationException("Close the game first: the update replaces the engine.");
                var zip = Path.Combine(work, "package.zip");
                await DownloadAsync(release.ZipUrl!, zip, release.ZipSize, progress);
                var unpacked = Path.Combine(work, "unpacked");
                var root = await Task.Run(() =>
                {
                    Extract(zip, unpacked, skipped);
                    return (Directory.GetFiles(unpacked, ExeName, SearchOption.AllDirectories).FirstOrDefault() ??
                            OldExeNames.Select(old => Directory.GetFiles(unpacked, old, SearchOption.AllDirectories).FirstOrDefault()).FirstOrDefault(f => f != null)) is { } found
                        ? Path.GetDirectoryName(found)
                        : throw new InvalidOperationException($"The package has no {ExeName}.");
                });
                if (Process.GetProcessesByName("bb-probe").Length > 0)
                    throw new InvalidOperationException("Close the game first: the update replaces the engine.");
                SdlPad.Unload(); // the Controls tab's SDL3.dll keeps it and out\libiconv-2.dll open
                await Task.Run(() => CopyPackage(root!, folder, self, skipped));
            }
            else
            {
                var url = release.ExeUrl ?? throw new InvalidOperationException("This release has no launcher download attached.");
                var exe = Path.Combine(work, ExeName);
                await DownloadAsync(url, exe, release.ExeSize, progress);
                if (new FileInfo(exe).Length < 100_000)
                    throw new InvalidOperationException("The download is too small to be the launcher.");
                await Task.Run(() => ReplaceSelf(self, exe));
            }
            Process.Start(new ProcessStartInfo(self) { UseShellExecute = true, WorkingDirectory = folder });
            return skipped;
        }

        /// <summary>Package folders the game runs without: a file there an antivirus will not let
        /// through (bbbridge, a new unsigned program that goes online, is the one seen) is left as it
        /// was and reported, instead of stopping the whole update.</summary>
        private static readonly string[] Optional = { "bbbridge" };

        private static bool IsOptional(string relative) =>
            Optional.Any(o => relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                .Contains(o, StringComparer.OrdinalIgnoreCase));

        /// <summary>An antivirus scanning a new file holds it for a moment: tried again for a few
        /// seconds before giving up.</summary>
        private static void Retry(Action action)
        {
            for (var attempt = 1; ; attempt++)
            {
                try { action(); return; }
                catch (Exception e) when (attempt < 6 && e is IOException or UnauthorizedAccessException)
                {
                    Thread.Sleep(500 * attempt);
                }
            }
        }

        // No inner exception: the update dialog shows GetBaseException(), which would drop the advice.
        private static Exception Blocked(string file, Exception e) => new InvalidOperationException(
            $"{e.Message}\n\nSomething is holding or removing {Path.GetFileName(file)}. Close the game and every " +
            "Nightwatch Co-op window, start the launcher again and update. If it happens again, an antivirus is most " +
            "likely the cause: allow the Nightwatch Co-op folder in it and update again.");

        private static void Extract(string zip, string to, List<string> skipped)
        {
            using var archive = ZipFile.OpenRead(zip);
            var root = Path.GetFullPath(to) + Path.DirectorySeparatorChar;
            foreach (var entry in archive.Entries)
            {
                var target = Path.GetFullPath(Path.Combine(to, entry.FullName));
                if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue; // outside the folder
                if (entry.FullName.EndsWith('/') || entry.FullName.EndsWith('\\'))
                {
                    Directory.CreateDirectory(target);
                    continue;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                try { Retry(() => entry.ExtractToFile(target, overwrite: true)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    if (!IsOptional(entry.FullName)) throw Blocked(target, e);
                    skipped.Add(entry.FullName.Replace('\\', '/'));
                }
            }
        }

        /// <summary>Copies a new package over this one, leaving the player's own files alone. The
        /// launcher and package.json (which says which engine is installed) go last. Originals stay
        /// in the update's work folder until every file is installed, so a failed copy can roll back.</summary>
        private static void CopyPackage(string from, string to, string self, List<string> skipped)
        {
            var selfTarget = Path.GetFullPath(self);
            var manifest = Path.GetFullPath(Path.Combine(to, "package.json"));
            var backup = from + ".backup";
            var replaced = new List<(string Target, string? Original)>();
            var created = new List<string>();
            void Restore((string Target, string? Original) entry)
            {
                if (entry.Original != null)
                    Retry(() => File.Move(entry.Original, entry.Target, overwrite: true));
                else
                    Retry(() => File.Delete(entry.Target));
            }
            int Last(string file) => Path.GetFullPath(Path.Combine(to, Path.GetRelativePath(from, file))) is var t &&
                string.Equals(t, selfTarget, StringComparison.OrdinalIgnoreCase) ? 2 :
                string.Equals(t, manifest, StringComparison.OrdinalIgnoreCase) ? 1 : 0;
            try
            {
                foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories).OrderBy(Last))
                {
                    var relative = Path.GetRelativePath(from, file);
                    var top = relative.Split(Path.DirectorySeparatorChar)[0];
                    if (Kept.Contains(top, StringComparer.OrdinalIgnoreCase)) continue;

                    var target = Path.Combine(to, relative);
                    var recorded = false;
                    try
                    {
                        if (string.Equals(Path.GetFullPath(target), selfTarget, StringComparison.OrdinalIgnoreCase))
                        {
                            ReplaceSelf(self, file);
                            continue;
                        }
                        var missing = new Stack<string>();
                        for (var dir = Path.GetDirectoryName(target)!; !Directory.Exists(dir); dir = Path.GetDirectoryName(dir)!)
                            missing.Push(dir);
                        while (missing.Count > 0)
                        {
                            var dir = missing.Pop();
                            Directory.CreateDirectory(dir);
                            created.Add(dir);
                        }
                        string? original = null;
                        if (File.Exists(target))
                        {
                            original = Path.Combine(backup, relative);
                            Directory.CreateDirectory(Path.GetDirectoryName(original)!);
                            Retry(() => File.Move(target, original));
                        }
                        replaced.Add((target, original));
                        recorded = true;
                        Retry(() => File.Copy(file, target, overwrite: true));
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                    {
                        if (!IsOptional(relative)) throw Blocked(file, e);
                        if (recorded)
                        {
                            Restore(replaced[^1]);
                            replaced.RemoveAt(replaced.Count - 1);
                        }
                        skipped.Add(relative.Replace('\\', '/'));
                    }
                }
            }
            catch (Exception error)
            {
                // Keep trying the rest if one restore is blocked, and retain its backup for recovery.
                foreach (var entry in replaced.AsEnumerable().Reverse())
                {
                    try { Restore(entry); }
                    catch (Exception e) { error.Data[entry.Target] = e; }
                }
                foreach (var dir in created.AsEnumerable().Reverse())
                    try { Directory.Delete(dir); } catch (Exception e) { error.Data[dir] = e; }
                throw;
            }
            try { if (Directory.Exists(backup)) Directory.Delete(backup, recursive: true); } catch (Exception) { }
        }

        private static void ReplaceSelf(string self, string replacement)
        {
            // The exe an earlier update moved aside can still be running (a launcher window left open)
            // or be read-only, and then cannot be deleted ("Access to the path ... .old is denied"):
            // this update moves the running exe to a name of its own instead. CleanUp removes them all.
            var aside = self + ".old";
            if (File.Exists(aside) && !TryDelete(aside))
                aside = self + "." + DateTime.Now.ToString("yyyyMMddHHmmss") + ".old";
            File.Move(self, aside);
            try
            {
                File.Copy(replacement, self);
            }
            catch
            {
                Retry(() => File.Delete(self)); // a failed copy may have left a partial exe
                Retry(() => File.Move(aside, self)); // put the working one back
                throw;
            }
        }

        private static async Task DownloadAsync(string url, string path, long expected, IProgress<double>? progress)
        {
            using var http = Client(TimeSpan.FromMinutes(30));
            using var response = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? expected;
            await using var input = await response.Content.ReadAsStreamAsync();
            await using var output = File.Create(path);
            var buffer = new byte[81920];
            long done = 0;
            int read;
            while ((read = await input.ReadAsync(buffer)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, read));
                done += read;
                if (total > 0) progress?.Report((double)done / total);
            }
        }

        /// <summary>Removes the exes updates moved aside (they were still running then).</summary>
        public static void CleanUp()
        {
            try
            {
                if (Environment.ProcessPath is not { } self) return;
                var name = Path.GetFileName(self);
                foreach (var old in Directory.GetFiles(Path.GetDirectoryName(self)!, name + ".*old"))
                    TryDelete(old); // still in use for a moment: removed next time
            }
            catch (Exception)
            {
            }
        }

        /// <summary>Deletes a file, read-only or not; false when it cannot be deleted now.</summary>
        private static bool TryDelete(string path)
        {
            try
            {
                var attributes = File.GetAttributes(path);
                if (attributes.HasFlag(FileAttributes.ReadOnly))
                    File.SetAttributes(path, attributes & ~FileAttributes.ReadOnly);
                File.Delete(path);
                return true;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return false;
            }
        }

        private static HttpClient Client(TimeSpan timeout)
        {
            var http = new HttpClient { Timeout = timeout };
            http.DefaultRequestHeaders.UserAgent.ParseAdd($"BloodborneLauncher/{Current}");
            http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            return http;
        }
    }
}
