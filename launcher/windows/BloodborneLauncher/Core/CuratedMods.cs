using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>One downloadable version of the curated mod (a manifest entry).</summary>
    public sealed record ModVersion(string Version, string Url, string Sha256, long Size, string Content);

    /// <summary>The manifest tools/mod-release.py writes (schema 1).</summary>
    public sealed record ModManifest(string Name, string Page, string Latest, IReadOnlyList<ModVersion> Versions)
    {
        public ModVersion? Find(string version) =>
            Versions.FirstOrDefault(v => string.Equals(v.Version, version, StringComparison.OrdinalIgnoreCase));

        public ModVersion Newest => Find(Latest) ?? Versions[0];
    }

    public enum InstalledKind { None, Known, Unrecognised }

    /// <summary>What the mods folder holds under the name Nocturne.</summary>
    public sealed record InstalledMod(InstalledKind Kind, string? Version = null);

    public enum JoinKind { Join, GetNocturne, MismatchJoinAnyway }

    /// <summary>What a guest should do before joining a host (<see cref="CuratedMods.JoinAdvice"/>).</summary>
    public readonly record struct JoinChoice(JoinKind Kind, string? Version = null);

    /// <summary>The guest's own mods: its game mode, its Nocturne version (null when none is installed) and how many mods
    /// of "My mods" are on.</summary>
    public sealed record LocalMods(string Mode, string? NocturneVersion, int EnabledCount);

    /// <summary>
    /// The game mode (Standard / Nocturne / My mods) and the curated mod Nocturne: its public manifest, the content hash
    /// (the same as tools/mod-release.py content_hash), the download and install, and the mods.json each mode needs.
    /// </summary>
    public static class CuratedMods
    {
        public const string DefaultManifestUrl = "https://nightwatch-mods.mods-cdn.workers.dev/nocturne/manifest.json";

        /// <summary>The manifest address; BB_NOCTURNE_MANIFEST replaces it. Empty: no download.</summary>
        public static string ManifestUrl =>
            Environment.GetEnvironmentVariable("BB_NOCTURNE_MANIFEST") is { Length: > 0 } url ? url.Trim() : DefaultManifestUrl;

        public const string FolderName = "Nocturne";
        public const string Marker = "nocturne.json";
        public const string ProfileKey = "mod_profile";
        public const string SavedKey = "my_mods_saved";
        private const string WrittenKey = "mod_profile_written";

        private static readonly string[] Loose = { ".txt", ".md", ".jpg", ".png", ".ini", ".toml" };

        // ---------------------------------------------------------------- manifest

        public static ModManifest ParseManifest(string json)
        {
            try
            {
                if (JsonNode.Parse(json) is not JsonObject root) throw new FormatException("not an object");
                if (Number(root["schema"]) != 1) throw new FormatException("schema is not 1");
                var versions = new List<ModVersion>();
                if (root["versions"] is not JsonArray list || list.Count == 0) throw new FormatException("no versions");
                foreach (var node in list)
                {
                    if (node is not JsonObject v) throw new FormatException("a version is not an object");
                    var url = Text(v["url"], "url");
                    if (!IsHttps(url)) throw new FormatException("a download address is not https");
                    var size = Number(v["size"]);
                    if (size is null or <= 0) throw new FormatException("a version has no size");
                    versions.Add(new ModVersion(Text(v["version"], "version"), url, Hex(v["sha256"], "sha256"), size.Value, Hex(v["content"], "content")));
                }
                var latest = Text(root["latest"], "latest");
                if (!versions.Any(v => string.Equals(v.Version, latest, StringComparison.OrdinalIgnoreCase)))
                    throw new FormatException("latest is not in versions");
                if (root["name"] is not JsonValue || root["page"] is not JsonValue) throw new FormatException("name or page missing");
                return new ModManifest(Text(root["name"], "name"), root["page"]!.GetValue<string>() ?? "", latest, versions);
            }
            catch (Exception e) when (e is FormatException or JsonException or InvalidOperationException)
            {
                throw new FormatException(L.F($"The Nocturne download list is not valid: {e.Message}"), e);
            }

            static long? Number(JsonNode? node) =>
                node is JsonValue v && v.TryGetValue<long>(out var n) ? n : null;

            static string Text(JsonNode? node, string name) =>
                node is JsonValue v && v.TryGetValue<string>(out var s) && !string.IsNullOrWhiteSpace(s) ? s.Trim()
                    : throw new FormatException(name + " missing");

            static string Hex(JsonNode? node, string name)
            {
                var s = Text(node, name);
                if (s.Length != 64 || !s.All(Uri.IsHexDigit)) throw new FormatException(name + " is not a SHA-256");
                return s.ToLowerInvariant();
            }
        }

        public static bool IsHttps(string url) =>
            Uri.TryCreate(url, UriKind.Absolute, out var uri) && uri.Scheme == Uri.UriSchemeHttps;

        /// <summary>The manifest from the network, or null when no address is set.</summary>
        public static async Task<ModManifest?> FetchManifestAsync(CancellationToken ct = default)
        {
            var url = ManifestUrl;
            if (url.Length == 0) return null;
            if (!IsHttps(url)) throw new FormatException(L.T("The Nocturne download address is not https."));
            using var http = Updater.Client(TimeSpan.FromSeconds(15));
            return ParseManifest(await http.GetStringAsync(url, ct));
        }

        // ---------------------------------------------------------------- content hash

        /// <summary>The folder whose files keep their path: the one holding dvdroot_ps4, else the mod folder
        /// (tools/mod-release.py content_root).</summary>
        internal static string ContentRoot(string folder)
        {
            var bases = new List<string> { folder, Path.Combine(folder, "app0") };
            bases.AddRange(Directory.GetDirectories(folder).OrderBy(d => Path.GetFileName(d), StringComparer.OrdinalIgnoreCase));
            foreach (var b in bases)
                if (Directory.Exists(b) && Directory.GetDirectories(b).Any(c =>
                        string.Equals(Path.GetFileName(c), "dvdroot_ps4", StringComparison.OrdinalIgnoreCase)))
                    return b;
            return folder;
        }

        private static bool Counted(string path) =>
            !Loose.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase) &&
            !string.Equals(Path.GetFileName(path), Marker, StringComparison.OrdinalIgnoreCase);

        /// <summary>The files that make up a mod, as (full path, lower-case '/'-separated path from the content root).</summary>
        internal static IEnumerable<(string Full, string Relative)> GameFiles(string folder)
        {
            var root = ContentRoot(folder);
            return new DirectoryInfo(root).EnumerateFiles("*", SearchOption.AllDirectories)
                .Where(f => Counted(f.FullName))
                .Select(f => (f.FullName, Path.GetRelativePath(root, f.FullName).Replace('\\', '/').ToLowerInvariant()));
        }

        /// <summary>SHA-256 over the sorted lines "path TAB sha256 of the file LF" (tools/mod-release.py content_hash).</summary>
        public static string ContentHash(string folder)
        {
            var lines = new List<string>();
            foreach (var (full, relative) in GameFiles(folder))
            {
                using var stream = File.OpenRead(full);
                lines.Add($"{relative}\t{Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant()}\n");
            }
            if (lines.Count == 0) throw new InvalidDataException(L.T("The folder holds no mod files."));
            lines.Sort(StringComparer.Ordinal);
            return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(string.Concat(lines)))).ToLowerInvariant();
        }

        private static readonly Dictionary<string, (long Stamp, int Count, string Hash)> HashCache = new(StringComparer.OrdinalIgnoreCase);

        /// <summary>ContentHash, kept per folder until a file in it changes (newest write time and file count).</summary>
        private static string CachedHash(string folder)
        {
            var full = Path.GetFullPath(folder);
            long stamp = 0;
            var count = 0;
            foreach (var f in new DirectoryInfo(full).EnumerateFiles("*", SearchOption.AllDirectories))
            {
                stamp = Math.Max(stamp, f.LastWriteTimeUtc.Ticks);
                count++;
            }
            lock (HashCache)
                if (HashCache.TryGetValue(full, out var hit) && hit.Stamp == stamp && hit.Count == count) return hit.Hash;
            var hash = ContentHash(full);
            lock (HashCache) HashCache[full] = (stamp, count, hash);
            return hash;
        }

        // ---------------------------------------------------------------- what is installed

        /// <summary>The Nocturne folder of the mods folder (any letter case), or null.</summary>
        public static string? FolderIn(string modsDir) =>
            Directory.Exists(modsDir)
                ? Directory.GetDirectories(modsDir).FirstOrDefault(d =>
                    string.Equals(Path.GetFileName(d), FolderName, StringComparison.OrdinalIgnoreCase))
                : null;

        /// <summary>Which version the Nocturne folder is: the one nocturne.json names when its content still matches, else the
        /// manifest version with the same content (a Nexus or hand install), else unrecognised.</summary>
        public static InstalledMod Installed(string modsDir, ModManifest? manifest = null)
        {
            if (FolderIn(modsDir) is not { } folder) return new InstalledMod(InstalledKind.None);
            string hash;
            try { hash = CachedHash(folder); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                return new InstalledMod(InstalledKind.Unrecognised);
            }
            try
            {
                var marker = Path.Combine(folder, Marker);
                if (File.Exists(marker) && JsonNode.Parse(File.ReadAllText(marker)) is JsonObject o &&
                    o["version"] is JsonValue v && v.TryGetValue<string>(out var version) && version.Length > 0 &&
                    o["content"] is JsonValue c && c.TryGetValue<string>(out var content) &&
                    string.Equals(content, hash, StringComparison.OrdinalIgnoreCase))
                    return new InstalledMod(InstalledKind.Known, version);
            }
            catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
            {
            }
            if (manifest?.Versions.FirstOrDefault(m => string.Equals(m.Content, hash, StringComparison.OrdinalIgnoreCase)) is { } match)
                return new InstalledMod(InstalledKind.Known, match.Version);
            return new InstalledMod(InstalledKind.Unrecognised);
        }

        // ---------------------------------------------------------------- install

        public static Task InstallAsync(ModVersion entry, string modsDir, IProgress<double>? progress, CancellationToken ct) =>
            InstallAsync(entry, modsDir, progress, ct, DownloadAsync);

        /// <summary>Downloads the zip to <paramref name="zipPath"/> (replaced in tests).</summary>
        internal delegate Task Fetch(ModVersion entry, string zipPath, IProgress<double>? progress, CancellationToken ct);

        /// <summary>Download, check size and SHA-256, extract beside the old folder, check the content hash, write nocturne.json,
        /// then swap. Whatever fails, the old Nocturne folder stays.</summary>
        internal static async Task InstallAsync(ModVersion entry, string modsDir, IProgress<double>? progress, CancellationToken ct, Fetch fetch)
        {
            if (!IsHttps(entry.Url)) throw new InvalidDataException(L.T("The Nocturne download address is not https."));
            modsDir = Path.TrimEndingDirectorySeparator(Path.GetFullPath(modsDir));
            Directory.CreateDirectory(modsDir);
            var data = Path.GetDirectoryName(modsDir) ?? modsDir;
            var id = Guid.NewGuid().ToString("N")[..8];
            var zip = Path.Combine(data, $".nocturne-{id}.zip");
            var fresh = Path.Combine(modsDir, $".nocturne-new-{id}");
            var aside = Path.Combine(modsDir, $".nocturne-old-{id}");
            try
            {
                await fetch(entry, zip, progress, ct);
                await Task.Run(() =>
                {
                    Verify(entry, zip);
                    Extract(zip, fresh, ct);
                    var content = ContentHash(fresh);
                    if (!string.Equals(content, entry.Content, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException(L.T("The downloaded mod does not match its list entry."));
                    File.WriteAllText(Path.Combine(fresh, Marker),
                        new JsonObject { ["version"] = entry.Version, ["content"] = content }.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
                    Swap(modsDir, fresh, aside);
                }, ct);
            }
            finally
            {
                TryDelete(zip, false);
                TryDelete(fresh, true);
            }
        }

        private static async Task DownloadAsync(ModVersion entry, string zipPath, IProgress<double>? progress, CancellationToken ct)
        {
            using var http = Updater.Client(TimeSpan.FromMinutes(30));
            using var response = await http.GetAsync(entry.Url, HttpCompletionOption.ResponseHeadersRead, ct);
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? entry.Size;
            await using var input = await response.Content.ReadAsStreamAsync(ct);
            await using var output = File.Create(zipPath);
            var buffer = new byte[81920];
            long done = 0;
            int read;
            while ((read = await input.ReadAsync(buffer, ct)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, read), ct);
                done += read;
                if (total > 0) progress?.Report(Math.Min(1.0, (double)done / total));
            }
        }

        private static void Verify(ModVersion entry, string zip)
        {
            if (new FileInfo(zip).Length != entry.Size)
                throw new InvalidDataException(L.T("The download is not the expected size: it is damaged or incomplete."));
            using var stream = File.OpenRead(zip);
            if (!string.Equals(Convert.ToHexString(SHA256.HashData(stream)), entry.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(L.T("The download is damaged: its checksum differs from the list."));
        }

        private static void Extract(string zip, string destination, CancellationToken ct)
        {
            using var archive = ZipFile.OpenRead(zip);
            var root = Path.GetFullPath(destination) + Path.DirectorySeparatorChar;
            // Every entry is checked before any is written: nothing may land outside the folder (zip-slip).
            foreach (var e in archive.Entries)
                if (!Path.GetFullPath(Path.Combine(destination, e.FullName)).StartsWith(root, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException(L.T("The download holds a file outside its folder and was refused."));
            Directory.CreateDirectory(destination);
            foreach (var e in archive.Entries)
            {
                ct.ThrowIfCancellationRequested();
                var path = Path.GetFullPath(Path.Combine(destination, e.FullName));
                if (e.Name.Length == 0)
                {
                    Directory.CreateDirectory(path);
                    continue;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                e.ExtractToFile(path, true);
            }
        }

        private static void Swap(string modsDir, string fresh, string aside)
        {
            var existing = FolderIn(modsDir);
            if (existing != null) Directory.Move(existing, aside);
            try
            {
                Directory.Move(fresh, Path.Combine(modsDir, FolderName));
            }
            catch
            {
                if (existing != null)
                    try { Directory.Move(aside, existing); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
                throw;
            }
            if (existing != null) TryDelete(aside, true);
        }

        private static void TryDelete(string path, bool folder)
        {
            try
            {
                if (folder) { if (Directory.Exists(path)) Directory.Delete(path, true); }
                else if (File.Exists(path)) File.Delete(path);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
            }
        }

        // ---------------------------------------------------------------- game mode

        /// <summary>The setting's mode: standard, nocturne or my (anything else is my).</summary>
        public static string Mode(LauncherSettings s) => s.Str(ProfileKey) is "standard" or "nocturne" ? s.Str(ProfileKey) : "my";

        /// <summary>Writes mods.json for the game mode; call right before every game start (new folders appear enabled in
        /// Mods.Load, which would otherwise break Nocturne mode). Leaving My mods saves its list into my_mods_saved, coming back
        /// restores it. Changes settings in memory only: true when the caller should save them.</summary>
        public static bool Apply(LauncherSettings s, string modsDir)
        {
            if (Paths.DataDir.Length == 0 || !Directory.Exists(modsDir)) return false;
            var mode = Mode(s);
            var written = s.Str(WrittenKey) is { Length: > 0 } w ? w : "my";
            var current = Mods.Load(modsDir);
            var changed = false;
            if (mode != "my")
            {
                if (written == "my")
                {
                    s.Set(SavedKey, SavedList(current));
                    changed = true;
                }
                var keep = mode == "nocturne" ? current.FirstOrDefault(m =>
                    string.Equals(m.Name, FolderName, StringComparison.OrdinalIgnoreCase))?.Name : null;
                Mods.Save(current.Select(m => m with { Enabled = keep != null && m.Name == keep }));
            }
            else if (written != "my" && ReadSaved(s.Str(SavedKey)) is { } saved)
            {
                var available = Mods.Discover(modsDir);
                var order = saved.Order.Where(available.Contains).Concat(available).Distinct();
                Mods.Save(order.Select(n => new Mods.Entry(n, !saved.Disabled.Contains(n))));
            }
            if (written != mode)
            {
                s.Set(WrittenKey, mode);
                changed = true;
            }
            return changed;
        }

        // The My mods list as saved: its load order and the disabled ones. A folder that was not in mods.json's order yet (Nocturne
        // right after its install) is saved off, so it does not join My mods by Mods.Load's rule for new folders.
        private static string SavedList(List<Mods.Entry> current)
        {
            var known = new HashSet<string>();
            try
            {
                if (File.Exists(Mods.ProfilePath) && JsonNode.Parse(File.ReadAllText(Mods.ProfilePath)) is JsonObject p && p["order"] is JsonArray a)
                    known = a.Select(n => n?.GetValue<string>() ?? "").ToHashSet(StringComparer.OrdinalIgnoreCase);
            }
            catch (Exception e) when (e is IOException or JsonException or InvalidOperationException)
            {
            }
            bool Off(Mods.Entry m) => !m.Enabled || (string.Equals(m.Name, FolderName, StringComparison.OrdinalIgnoreCase) && !known.Contains(m.Name));
            return new JsonObject
            {
                ["order"] = new JsonArray(current.Select(m => (JsonNode?)m.Name).ToArray()),
                ["disabled"] = new JsonArray(current.Where(Off).Select(m => (JsonNode?)m.Name).ToArray()),
            }.ToJsonString();
        }

        private static (List<string> Order, HashSet<string> Disabled)? ReadSaved(string json)
        {
            try
            {
                if (json.Length > 0 && JsonNode.Parse(json) is JsonObject o)
                    return (o["order"]?.AsArray().Select(n => n?.GetValue<string>() ?? "").ToList() ?? new List<string>(),
                        o["disabled"]?.AsArray().Select(n => n?.GetValue<string>() ?? "").ToHashSet() ?? new HashSet<string>());
            }
            catch (Exception e) when (e is JsonException or InvalidOperationException)
            {
            }
            return null;
        }

        /// <summary>The "My mods" count the Seamless page shows: the saved list's enabled mods while another mode is on, else mods.json's.</summary>
        public static int MyModsCount(LauncherSettings s, string modsDir)
        {
            var written = s.Str(WrittenKey) is { Length: > 0 } w ? w : "my";
            if (written != "my" && ReadSaved(s.Str(SavedKey)) is { } saved)
            {
                var available = Mods.Discover(modsDir);
                return available.Count(n => !saved.Disabled.Contains(n));
            }
            return Mods.Load(modsDir).Count(m => m.Enabled);
        }

        // ---------------------------------------------------------------- joining

        /// <summary>What a guest does before joining. hostLabel is empty (the host runs no mods), "nocturne@1.3" or "mods:&lt;8 hex&gt;".
        /// Any host Nocturne version is joinable: the one the guest lacks is fetched (an older one too). Any other label is a
        /// mismatch the guest can still join through when it has mods on.</summary>
        public static JoinChoice JoinAdvice(string? hostLabel, LocalMods local)
        {
            var label = (hostLabel ?? "").Trim();
            if (label.StartsWith("nocturne@", StringComparison.OrdinalIgnoreCase) && label.Length > "nocturne@".Length)
            {
                var version = label["nocturne@".Length..];
                return local.Mode == "nocturne" && string.Equals(local.NocturneVersion, version, StringComparison.OrdinalIgnoreCase)
                    ? new JoinChoice(JoinKind.Join)
                    : new JoinChoice(JoinKind.GetNocturne, version);
            }
            var modsOn = local.Mode == "nocturne" || (local.Mode == "my" && local.EnabledCount > 0);
            return modsOn ? new JoinChoice(JoinKind.MismatchJoinAnyway) : new JoinChoice(JoinKind.Join);
        }
    }
}
