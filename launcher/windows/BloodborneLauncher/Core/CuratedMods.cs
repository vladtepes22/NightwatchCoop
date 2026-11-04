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

    /// <summary>The launcher's start-up Nocturne popup (<see cref="CuratedMods.StartNewsOf"/>).</summary>
    public enum StartNews { None, Try, Update }

    /// <summary>Join: nothing to do. GetNocturne: switch to the host's Nocturne version (downloaded when missing).
    /// NocturneNotAvailable: the host's version is neither installed nor in the manifest. Modded: the host runs other mods
    /// (label only shown, nothing fetched). MismatchJoinAnyway: the host runs no mods and this guest does. All but Join
    /// still allow joining: the game's party mod hash decides.</summary>
    public enum JoinKind { Join, GetNocturne, NocturneNotAvailable, Modded, MismatchJoinAnyway }

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
        /// <summary>Nocturne's page on Nexus Mods (the Workshop's Nocturne card links to it).</summary>
        public const string NexusPage = "https://www.nexusmods.com/bloodborne/mods/747";
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

        /// <summary>The folder name the launcher gives its Nocturne install, "Nocturne v1.0.1" (user, 4 Nov), so the mods folder
        /// shows the version. The co-op hash never sees it (engine/start/mods.cpp hashes paths inside the mod). A version with
        /// other characters than a-z 0-9 . _ + - keeps the plain name.</summary>
        public static string FolderFor(string version) =>
            version.Length is > 0 and <= 32 && version.All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '+' or '-') && !version.EndsWith('.')
                ? $"{FolderName} v{version}" : FolderName;

        /// <summary>A Nocturne folder name: "Nocturne" (older launchers) or "Nocturne v..." (this launcher, the Nexus zip), any case.</summary>
        public static bool IsNocturneName(string name) =>
            string.Equals(name, FolderName, StringComparison.OrdinalIgnoreCase) ||
            name.StartsWith(FolderName + " v", StringComparison.OrdinalIgnoreCase);

        /// <summary>The Nocturne folder of the mods folder, or null; with several, the one the launcher installed (nocturne.json).</summary>
        public static string? FolderIn(string modsDir)
        {
            if (!Directory.Exists(modsDir)) return null;
            var all = Directory.GetDirectories(modsDir).Where(d => IsNocturneName(Path.GetFileName(d)))
                .OrderBy(d => Path.GetFileName(d), StringComparer.OrdinalIgnoreCase).ToList();
            return all.FirstOrDefault(d => File.Exists(Path.Combine(d, Marker))) ?? all.FirstOrDefault();
        }

        /// <summary>Gives the launcher's own Nocturne install (nocturne.json matching its files) its version name, then points the
        /// entries of mods.json and the saved My mods list at the current Nocturne folder, keeping load order and on/off (a renamed
        /// folder would otherwise come back as a new, enabled mod). Call with the game closed. True when settings changed.</summary>
        public static bool NameFolder(LauncherSettings s, string modsDir)
        {
            if (Paths.DataDir.Length == 0 || FolderIn(modsDir) is not { } folder) return false;
            if (Installed(modsDir) is { Kind: InstalledKind.Known, Version: { } version }) // no manifest: the marker only
            {
                var want = Path.Combine(modsDir, FolderFor(version));
                var caseOnly = string.Equals(folder, want, StringComparison.OrdinalIgnoreCase);
                if (!string.Equals(folder, want, StringComparison.Ordinal) && (caseOnly || !Directory.Exists(want)))
                    try
                    {
                        Directory.Move(folder, want);
                        folder = want;
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                    {
                        // a file in use: the next start renames it
                    }
            }
            var name = Path.GetFileName(folder);
            string Map(string n) => IsNocturneName(n) && !string.Equals(n, name, StringComparison.Ordinal) &&
                !Directory.Exists(Path.Combine(modsDir, n)) ? name : n;
            try
            {
                if (File.Exists(Mods.ProfilePath) && JsonNode.Parse(File.ReadAllText(Mods.ProfilePath)) is JsonObject p &&
                    Renamed(p, Map))
                    File.WriteAllText(Mods.ProfilePath, p.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            }
            catch (Exception e) when (e is IOException or JsonException or InvalidOperationException or UnauthorizedAccessException)
            {
            }
            try
            {
                if (s.Str(SavedKey) is { Length: > 0 } saved && JsonNode.Parse(saved) is JsonObject o && Renamed(o, Map))
                {
                    s.Set(SavedKey, o.ToJsonString());
                    return true;
                }
            }
            catch (Exception e) when (e is JsonException or InvalidOperationException)
            {
            }
            return false;
        }

        // An order/disabled list with each name mapped (duplicates dropped, the first place kept); true when it changed.
        private static bool Renamed(JsonObject list, Func<string, string> map)
        {
            var changed = false;
            foreach (var key in new[] { "order", "disabled" })
            {
                if (list[key] is not JsonArray a) continue;
                var before = a.Select(n => n?.GetValue<string>() ?? "").ToList();
                var after = before.Select(map).Distinct().ToList();
                if (before.SequenceEqual(after)) continue;
                list[key] = new JsonArray(after.Select(n => (JsonNode?)n).ToArray());
                changed = true;
            }
            return changed;
        }

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
                    Swap(modsDir, fresh, aside, entry.Version);
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

        private static void Swap(string modsDir, string fresh, string aside, string version)
        {
            var existing = FolderIn(modsDir);
            var target = Path.Combine(modsDir, FolderFor(version));
            if (Directory.Exists(target) && !string.Equals(target, existing, StringComparison.OrdinalIgnoreCase))
                throw new IOException(L.F($"Your mods folder already has a folder named {Path.GetFileName(target)}. Remove or rename it, then try again."));
            if (existing != null) Directory.Move(existing, aside);
            try
            {
                Directory.Move(fresh, target);
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
        /// <summary>Load mods: "standard" (No mods), "my" (My mods) or "nocturne" (Nocturne only). My mods with the old Load mods
        /// switch off (mods_enabled, before the dropdown) reads as No mods.</summary>
        public static string Mode(LauncherSettings s) => s.Str(ProfileKey) is "standard" or "nocturne" ? s.Str(ProfileKey)
            : s.Bool("mods_enabled") ? "my" : "standard";

        /// <summary>Writes mods.json for the game mode; call right before every game start (new folders appear enabled in
        /// Mods.Load, which would otherwise break Nocturne mode). Leaving My mods saves its list into my_mods_saved, coming back
        /// restores it. Changes settings in memory only: true when the caller should save them.</summary>
        public static bool Apply(LauncherSettings s, string modsDir)
        {
            if (Paths.DataDir.Length == 0 || !Directory.Exists(modsDir)) return false;
            var mode = Mode(s);
            var written = s.Str(WrittenKey) is { Length: > 0 } w ? w : "my";
            var changed = NameFolder(s, modsDir);
            var current = Mods.Load(modsDir);
            if (mode != "my")
            {
                if (written == "my")
                {
                    s.Set(SavedKey, SavedList(current));
                    changed = true;
                }
                var nocturne = FolderIn(modsDir) is { } f ? Path.GetFileName(f) : null;
                var keep = mode == "nocturne" ? current.FirstOrDefault(m =>
                    string.Equals(m.Name, nocturne, StringComparison.OrdinalIgnoreCase))?.Name : null;
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
            bool Off(Mods.Entry m) => !m.Enabled || (IsNocturneName(m.Name) && !known.Contains(m.Name));
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

        public const string LabelKey = "nocturne";

        /// <summary>The lobby label this host announces for Nocturne (BB_PARTY_MODS_LABEL): "nocturne@1.0" in Nocturne mode with
        /// an install whose content matches its nocturne.json, else null (the game then announces "mods:" + its mod hash).</summary>
        public static string? HostLabel(LauncherSettings s, string modsDir)
        {
            if (Mode(s) != "nocturne") return null;
            var installed = Installed(modsDir);
            if (installed.Kind != InstalledKind.Known || installed.Version is not { Length: > 0 } version) return null;
            var label = LabelKey + "@" + version.ToLowerInvariant();
            return VpnLobby.ValidModsLabel(System.Text.Encoding.ASCII.GetBytes(label)) ? label : null;
        }

        /// <summary>The Nocturne version a lobby label names, or null. The label is untrusted: it is only split into parts
        /// ('+') and compared, never used as a path or an address; the version must itself be a plain one (digits, letters, dots).</summary>
        public static string? NocturneOf(string? hostLabel)
        {
            foreach (var part in (hostLabel ?? "").Split('+'))
            {
                if (!part.StartsWith(LabelKey + "@", StringComparison.Ordinal)) continue;
                var version = part[(LabelKey.Length + 1)..];
                if (version.Length is > 0 and <= 16 && version.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '.' or '-'))
                    return version;
            }
            return null;
        }

        /// <summary>What the launcher tells at its start, whatever the game mode (user, 4 Nov: advertise Nocturne): Try when
        /// Nocturne is not installed, Update when a known older version is, each once per manifest version
        /// (<paramref name="toldVersion"/>, the last one told). Nothing without a manifest or for an unrecognised folder
        /// (the player's own copy).</summary>
        public static StartNews StartNewsOf(ModManifest? manifest, InstalledMod installed, string? toldVersion)
        {
            if (manifest == null || string.Equals(toldVersion, manifest.Newest.Version, StringComparison.OrdinalIgnoreCase))
                return StartNews.None;
            return installed.Kind switch
            {
                InstalledKind.None => StartNews.Try,
                InstalledKind.Known when !string.Equals(installed.Version, manifest.Newest.Version, StringComparison.OrdinalIgnoreCase)
                    => StartNews.Update,
                _ => StartNews.None,
            };
        }

        /// <summary>What a guest does before joining (visy's terms, 4 Nov): the label is a key only. A Nocturne version is
        /// fetched only when the guest's built-in manifest offers it (<paramref name="offered"/>, null when it could not be
        /// read) or the guest already has it; else "not available". Any other label only shows "modded".</summary>
        public static JoinChoice JoinAdvice(string? hostLabel, LocalMods local, IReadOnlyCollection<string>? offered = null)
        {
            var label = (hostLabel ?? "").Trim();
            if (NocturneOf(label) is { } version)
            {
                if (local.Mode == "nocturne" && string.Equals(local.NocturneVersion, version, StringComparison.OrdinalIgnoreCase))
                    return new JoinChoice(JoinKind.Join);
                var have = string.Equals(local.NocturneVersion, version, StringComparison.OrdinalIgnoreCase) ||
                           offered?.Contains(version, StringComparer.OrdinalIgnoreCase) == true;
                return new JoinChoice(have ? JoinKind.GetNocturne : JoinKind.NocturneNotAvailable, version);
            }
            if (label.Length > 0) return new JoinChoice(JoinKind.Modded);
            var modsOn = local.Mode == "nocturne" || (local.Mode == "my" && local.EnabledCount > 0);
            return modsOn ? new JoinChoice(JoinKind.MismatchJoinAnyway) : new JoinChoice(JoinKind.Join);
        }

        /// <summary>The guest's side of "Use Nocturne X and join": installs that version from the manifest when the folder holds
        /// another one, then sets Nocturne mode and writes mods.json. Throws with a message the page shows.</summary>
        public static async Task UseNocturneAsync(LauncherSettings s, string modsDir, string version, ModManifest? manifest,
            IProgress<double>? progress, CancellationToken ct = default)
        {
            var installed = await Task.Run(() => Installed(modsDir, manifest), ct);
            if (!(installed.Kind == InstalledKind.Known && string.Equals(installed.Version, version, StringComparison.OrdinalIgnoreCase)))
            {
                if (manifest?.Find(version) is not { } entry)
                    throw new InvalidDataException(L.F($"Nocturne {version} is not available for download."));
                await InstallAsync(entry, modsDir, progress, ct);
            }
            s.Set(ProfileKey, "nocturne");
            Apply(s, modsDir);
        }

        /// <summary>The local side for JoinAdvice: mode, installed Nocturne version (any known one) and My mods' count.</summary>
        public static LocalMods Local(LauncherSettings s, string modsDir, ModManifest? manifest) =>
            new(Mode(s), Installed(modsDir, manifest) is { Kind: InstalledKind.Known } i ? i.Version : null, MyModsCount(s, modsDir));
    }
}
