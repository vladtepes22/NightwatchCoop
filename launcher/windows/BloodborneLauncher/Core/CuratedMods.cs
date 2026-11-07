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

    /// <summary>The Workshop's Nocturne popup (<see cref="CuratedMods.StartNewsOf"/>).</summary>
    public enum StartNews { None, Try, Update }

    /// <summary>Join: nothing to do. GetNocturne: switch to the host's Nocturne version (downloaded when missing).
    /// NocturneNotAvailable: the host's version is neither installed nor in the manifest. Modded: the host runs other mods
    /// whose hash differs from this PC's (label only shown, nothing fetched). ModdedUnchecked: the host runs other mods and
    /// this PC's hash is unknown (no game start since its mods last changed). MismatchJoinAnyway: the host runs no gameplay
    /// mods and this guest does. All but Join still allow joining: the game's party mod hash decides. A randomized world:
    /// BuildWorld builds the host's world here (Code, Check, and Version when it is built on Nocturne); WorldNewer: made by a
    /// newer randomizer (update the launcher); WorldOlder: made by an older randomizer, which this launcher no longer builds
    /// (the host makes it again); WorldNotAvailable: this launcher cannot build it; WorldModMissing: the world is built on a
    /// mod (named by content in the label) that no folder here has (Missing lists the keys).</summary>
    public enum JoinKind { Join, GetNocturne, NocturneNotAvailable, Modded, ModdedUnchecked, MismatchJoinAnyway, BuildWorld, WorldNewer, WorldNotAvailable, WorldOlder,
        WorldModMissing }

    /// <summary>What a guest should do before joining a host (<see cref="CuratedMods.JoinAdvice"/>). For a world: Code is the
    /// canonical world code, Check the host's input hash (8 hex, the world folder's last part). PatchesDiffer: the host's
    /// gameplay patches hash (its label's "patches:" part) is known and differs from this PC's, which the join check refuses
    /// as surely as other mods (a mod's own XML patch counts). Base: the world's base mods as the label names them, in load
    /// order ("nocturne@1.0+m:0a1b2c3d"; null for the game alone). Missing: the base keys no folder here has, comma-joined.</summary>
    public readonly record struct JoinChoice(JoinKind Kind, string? Version = null, string? Code = null, string? Check = null,
        bool PatchesDiffer = false, string? Base = null, string? Missing = null);

    /// <summary>The guest's own mods: its game mode, its Nocturne version (null when none is installed), how many mods
    /// of "My mods" are on, the first 8 hex of its party mods and patches hashes as a start would have them now
    /// (<see cref="PartyHashes"/>; null when unknown), the world folder world mode plays, whether this package can build
    /// worlds, the generator that made that world (its manifest's; null when unknown), what that world's base folders stand for
    /// now (<see cref="CuratedMods.BaseKeysOf"/>; null for the game alone, "?" when it can't be told), and the base keys this
    /// PC's mods can stand for, sorted and comma-joined (<see cref="CuratedMods.KnownBaseTokens"/>; null when not known yet).</summary>
    public sealed record LocalMods(string Mode, string? NocturneVersion, int EnabledCount, string? ModsHash = null, string? PatchesHash = null,
        string? World = null, bool CanBuildWorlds = false, int? WorldGenerator = null, string? WorldBase = null, string? BaseTokens = null);

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

        /// <summary>A base mod's identity in a world's lobby label ("m:" + its first 8 hex), whatever its folder is called:
        /// SHA-256 over the sorted lines "game path TAB sha256 of the file LF" of every file the game loads from the mod
        /// (engine/start/mods.cpp ModFiles: the files under its dvdroot_ps4, or all of a bare chr/ parts/ ... layout; paths
        /// from dvdroot_ps4, lower case, '/'), cosmetic files included. Two folders with the same hash give the generator the
        /// same inputs and the party mods hash (WritePartyModsFile) the same part, so a world built on either is one world.</summary>
        public static string LoadedHash(string folder)
        {
            var root = ContentRoot(folder);
            var top = Directory.GetDirectories(root).FirstOrDefault(d =>
                string.Equals(Path.GetFileName(d), "dvdroot_ps4", StringComparison.OrdinalIgnoreCase)) ?? root;
            var lines = new List<string>();
            foreach (var f in new DirectoryInfo(top).EnumerateFiles("*", SearchOption.AllDirectories))
            {
                using var stream = File.OpenRead(f.FullName);
                var relative = Path.GetRelativePath(top, f.FullName).Replace('\\', '/').ToLowerInvariant();
                lines.Add($"{relative}\t{Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant()}\n");
            }
            if (lines.Count == 0) throw new InvalidDataException(L.T("The folder holds no mod files."));
            lines.Sort(StringComparer.Ordinal);
            return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(string.Concat(lines)))).ToLowerInvariant();
        }

        private static readonly Dictionary<string, (long Stamp, int Count, string Hash)> HashCache = new(StringComparer.OrdinalIgnoreCase);

        /// <summary>ContentHash (or LoadedHash), kept per folder until a file in it changes (newest write time and file count).</summary>
        private static string CachedHash(string folder, bool loaded = false)
        {
            var full = Path.GetFullPath(folder);
            long stamp = 0;
            var count = 0;
            foreach (var f in new DirectoryInfo(full).EnumerateFiles("*", SearchOption.AllDirectories))
            {
                stamp = Math.Max(stamp, f.LastWriteTimeUtc.Ticks);
                count++;
            }
            var key = (loaded ? "loaded|" : "content|") + full;
            lock (HashCache)
                if (HashCache.TryGetValue(key, out var hit) && hit.Stamp == stamp && hit.Count == count) return hit.Hash;
            var hash = loaded ? LoadedHash(full) : ContentHash(full);
            lock (HashCache) HashCache[key] = (stamp, count, hash);
            return hash;
        }

        // ---------------------------------------------------------------- base mods by content (randomized worlds)

        /// <summary>The Nocturne version this folder holds, whatever it is called: the one its nocturne.json names when the
        /// content still matches, else the manifest version with the same content; null otherwise (or unreadable).</summary>
        public static string? NocturneVersionIn(string folder, ModManifest? manifest = null)
        {
            string hash;
            try { hash = CachedHash(folder); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { return null; }
            try
            {
                var marker = Path.Combine(folder, Marker);
                if (File.Exists(marker) && JsonNode.Parse(File.ReadAllText(marker)) is JsonObject o &&
                    o["version"] is JsonValue v && v.TryGetValue<string>(out var version) && version.Length > 0 &&
                    o["content"] is JsonValue c && c.TryGetValue<string>(out var content) &&
                    string.Equals(content, hash, StringComparison.OrdinalIgnoreCase))
                    return version;
            }
            catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
            {
            }
            return manifest?.Versions.FirstOrDefault(m => string.Equals(m.Content, hash, StringComparison.OrdinalIgnoreCase))?.Version;
        }

        /// <summary>The label key a Nocturne version goes by ("nocturne@1.0.1"), or null when the version is not a plain one
        /// (such a copy is named by its content, "m:...").</summary>
        private static string? NocturneToken(string? version) =>
            version is { Length: > 0 and <= 16 } v && v.ToLowerInvariant() is var lower &&
            lower.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '.' or '-') ? LabelKey + "@" + lower : null;

        /// <summary>What a mod folder can stand for in a world label: "m:" + its LoadedHash's first 8 hex, and "nocturne@&lt;v&gt;"
        /// too when it is a recognised Nocturne (any folder name). Throws on an unreadable or empty folder.</summary>
        public static List<string> BaseTokensOf(string folder, ModManifest? manifest = null)
        {
            var tokens = new List<string>();
            if (NocturneToken(NocturneVersionIn(folder, manifest)) is { } nocturne) tokens.Add(nocturne);
            tokens.Add(ModToken + CachedHash(folder, loaded: true)[..8]);
            return tokens;
        }

        /// <summary>A world label's key for a base mod by content: "m:" + 8 hex.</summary>
        public const string ModToken = "m:";

        /// <summary>The base mods' part of a world label, in load order (user, 5 Nov: base mods by content, not folder name):
        /// "nocturne@&lt;version&gt;" for a recognised Nocturne in any folder, "m:" + 8 hex of LoadedHash for any other mod,
        /// joined by '+'. Null for the game alone. Throws on a missing, unreadable or empty folder, or two mods of one content.</summary>
        public static string? BaseLabelOf(string modsDir, IReadOnlyList<string> baseMods, ModManifest? manifest = null)
        {
            if (baseMods.Count == 0) return null;
            var tokens = new List<string>();
            var nocturne = false;
            foreach (var name in baseMods)
            {
                var folder = Path.Combine(modsDir, name);
                if (!Directory.Exists(folder)) throw new DirectoryNotFoundException(name);
                var own = BaseTokensOf(folder, manifest);
                // One Nocturne by version; a second copy is named by its content.
                var token = !nocturne && own[0].StartsWith(LabelKey + "@", StringComparison.Ordinal) ? own[0] : own[^1];
                nocturne |= token.StartsWith(LabelKey + "@", StringComparison.Ordinal);
                if (tokens.Contains(token)) throw new InvalidDataException(L.T("Two base mods have the same files."));
                tokens.Add(token);
            }
            return string.Join('+', tokens);
        }

        /// <summary>What each base folder can stand for, in load order: its keys joined by '|', the folders by '+'
        /// ("nocturne@1.0|m:0a1b2c3d+m:ffff0000"); null for the game alone. Throws like <see cref="BaseLabelOf"/>.</summary>
        public static string? BaseKeysOf(string modsDir, IReadOnlyList<string> baseMods) =>
            baseMods.Count == 0 ? null : string.Join('+', baseMods.Select(name =>
                Directory.Exists(Path.Combine(modsDir, name)) ? string.Join('|', BaseTokensOf(Path.Combine(modsDir, name)))
                    : throw new DirectoryNotFoundException(name)));

        /// <summary>A label's base part names these folders (<see cref="BaseKeysOf"/>): as many, each standing for its key. A
        /// Nocturne copy the host named by content ("m:...") and this PC knows by version matches either way.</summary>
        public static bool BaseMatches(string? keys, string? basePart)
        {
            var have = (keys ?? "").Split('+', StringSplitOptions.RemoveEmptyEntries);
            var want = (basePart ?? "").Split('+', StringSplitOptions.RemoveEmptyEntries);
            return have.Length == want.Length && have.Zip(want).All(p => p.First.Split('|').Contains(p.Second, StringComparer.Ordinal));
        }

        /// <summary>The mod folders here that can stand for each label key, in Mods.Discover order (worlds left out): key to
        /// folder names. Unreadable folders are skipped.</summary>
        public static Dictionary<string, List<string>> BaseFolders(string modsDir, ModManifest? manifest = null)
        {
            var found = new Dictionary<string, List<string>>(StringComparer.Ordinal);
            if (!Directory.Exists(modsDir)) return found;
            foreach (var name in Mods.Discover(modsDir))
            {
                if (Randomizer.Parse(name) != null) continue;
                try
                {
                    foreach (var token in BaseTokensOf(Path.Combine(modsDir, name), manifest))
                        (found.TryGetValue(token, out var list) ? list : found[token] = new List<string>()).Add(name);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
                {
                }
            }
            return found;
        }

        /// <summary>The local folders for a world label's base part, in its order, or null with the keys no folder here has.</summary>
        public static (List<string>? Folders, List<string> Missing) ResolveBase(string modsDir, string? basePart, ModManifest? manifest = null)
        {
            var tokens = (basePart ?? "").Split('+', StringSplitOptions.RemoveEmptyEntries);
            if (tokens.Length == 0) return (new List<string>(), new List<string>());
            var have = BaseFolders(modsDir, manifest);
            var folders = new List<string>();
            var missing = new List<string>();
            foreach (var token in tokens)
            {
                var pick = have.TryGetValue(token, out var list) ? list.FirstOrDefault(n => !folders.Contains(n)) : null;
                if (pick == null) missing.Add(token);
                else folders.Add(pick);
            }
            return (missing.Count == 0 ? folders : null, missing);
        }

        private static readonly object TokensGate = new();
        private static (string Dir, string Stamp, string Tokens)? _tokens;
        private static Task? _tokensRun;

        /// <summary>The label keys this PC's mods can stand for ("m:..." and "nocturne@..."), sorted and comma-joined, as last
        /// worked out in the background; null until that is done for the mods as they are now (the lobby asks on every refresh
        /// while a host's world names a base mod by content).</summary>
        public static string? KnownBaseTokens(string modsDir)
        {
            string stamp;
            try
            {
                var text = new StringBuilder();
                if (Directory.Exists(modsDir))
                    foreach (var name in Mods.Discover(modsDir))
                    {
                        if (Randomizer.Parse(name) != null) continue;
                        long newest = 0;
                        var count = 0;
                        foreach (var f in new DirectoryInfo(Path.Combine(modsDir, name)).EnumerateFiles("*", SearchOption.AllDirectories))
                        {
                            newest = Math.Max(newest, f.LastWriteTimeUtc.Ticks);
                            count++;
                        }
                        text.Append(name).Append('|').Append(newest).Append('|').Append(count).Append('\n');
                    }
                stamp = text.ToString();
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
            lock (TokensGate)
            {
                if (_tokens is { } t && t.Dir == modsDir && t.Stamp == stamp) return t.Tokens;
                _tokensRun ??= Task.Run(() =>
                {
                    string? tokens = null;
                    try { tokens = string.Join(",", BaseFolders(modsDir).Keys.Order(StringComparer.Ordinal)); }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
                    lock (TokensGate)
                    {
                        if (tokens != null) _tokens = (modsDir, stamp, tokens);
                        _tokensRun = null;
                    }
                });
                return null;
            }
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
            return NocturneVersionIn(folder, manifest) is { } version ? new InstalledMod(InstalledKind.Known, version)
                : new InstalledMod(InstalledKind.Unrecognised);
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

        /// <summary>Load mods: "standard" (No mods), "my" (My mods), "nocturne" (Nocturne only) or "world" (a randomized world,
        /// Randomizer.Use). My mods with the old Load mods switch off (mods_enabled, before the dropdown) reads as No mods.</summary>
        public static string Mode(LauncherSettings s) => s.Str(ProfileKey) is "standard" or "nocturne" or "world" ? s.Str(ProfileKey)
            : s.Bool("mods_enabled") ? "my" : "standard";

        /// <summary>The game loads mods for this start (BB_MODS_ENABLED): every choice but No mods.</summary>
        public static bool ModsOn(LauncherSettings s) => Mode(s) != "standard";

        /// <summary>Writes mods.json for the game mode; call right before every game start (new folders appear enabled in
        /// Mods.Load, which would otherwise break Nocturne mode). Leaving My mods saves its list into my_mods_saved, coming back
        /// restores it. Changes settings in memory only: true when the caller should save them.</summary>
        public static bool Apply(LauncherSettings s, string modsDir)
        {
            if (Paths.DataDir.Length == 0 || !Directory.Exists(modsDir)) return false;
            var changed = NameFolder(s, modsDir);
            // A world that is gone (deleted, or its manifest unreadable) leaves world mode for the mode before it.
            var world = Mode(s) == "world" ? WorldLoad(modsDir, s.Str(Randomizer.WorldKey)) : null;
            if (Mode(s) == "world" && world == null)
            {
                Randomizer.StopUsing(s);
                changed = true;
            }
            var mode = Mode(s);
            var written = s.Str(WrittenKey) is { Length: > 0 } w ? w : "my";
            var current = Mods.Load(modsDir);
            if (mode != "my")
            {
                if (written == "my")
                {
                    s.Set(SavedKey, SavedList(current));
                    changed = true;
                }
                // The load order: the kept mods in their order (a world's base as it was built), the world last, the rest off.
                var nocturne = FolderIn(modsDir) is { } f ? Path.GetFileName(f) : null;
                var keep = mode == "nocturne"
                    ? current.Where(m => string.Equals(m.Name, nocturne, StringComparison.OrdinalIgnoreCase)).Select(m => m.Name).Take(1).ToList()
                    : world ?? new List<string>();
                Mods.Save(keep.Select(n => new Mods.Entry(n, true)).Concat(current.Where(m => !keep.Contains(m.Name)).Select(m => m with { Enabled = false })));
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

        // World mode's load order: the world's base mods as its manifest lists them, then the world. Null when the world, its
        // manifest or one of its base mods is missing (it would load as another world).
        private static List<string>? WorldLoad(string modsDir, string folderName)
        {
            if (Randomizer.Parse(folderName) == null) return null;
            var folder = Path.Combine(modsDir, folderName);
            if (!Directory.Exists(folder) || Randomizer.ReadManifest(folder) is not { } manifest) return null;
            var available = Mods.Discover(modsDir);
            if (manifest.Mods.Any(n => !available.Contains(n))) return null;
            return manifest.Mods.Append(folderName).ToList();
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
            if (Mode(s) == "world") return WorldHostLabel(modsDir, s.Str(Randomizer.WorldKey));
            if (Mode(s) != "nocturne") return null;
            var installed = Installed(modsDir);
            if (installed.Kind != InstalledKind.Known || installed.Version is not { Length: > 0 } version) return null;
            var label = LabelKey + "@" + version.ToLowerInvariant();
            return VpnLobby.ValidModsLabel(System.Text.Encoding.ASCII.GetBytes(label)) ? label : null;
        }

        /// <summary>A world's lobby label (its manifest's lobby_label), only when everything in it holds here: the folder name,
        /// code and input hash agree, the code decodes, the generator is ours, and its base is shareable and is what the
        /// label says (the game alone, or this verified Nocturne). Else null: the game announces "mods:" + its hash.</summary>
        internal static string? WorldHostLabel(string modsDir, string folderName)
        {
            if (Randomizer.Parse(folderName) is not { } world || WorldLoad(modsDir, folderName) == null) return null;
            if (Randomizer.ReadManifest(Path.Combine(modsDir, folderName)) is not { } m || m.Generator != Randomizer.GeneratorMajor) return null;
            if (WorldCode.Decode(m.Code) is not { Kind: WorldCodeKind.Ok, Canonical: { } code } || code != m.Code || code != world.Code) return null;
            if (!m.InputHash.StartsWith(world.Hash, StringComparison.Ordinal)) return null;
            var (shareable, baseLabel) = Randomizer.BaseLabel(modsDir, m.Mods);
            if (!shareable || baseLabel != m.BaseLabel) return null;
            var expected = (baseLabel != null ? baseLabel + "+" : "") + $"rando{Randomizer.GeneratorMajor}:{code.ToLowerInvariant()}+w:{world.Hash}" +
                (m.GeneratorBuild is { } build ? "+" + BuildKey + build[..8] : "");
            // generate.lobby_label: "+i:" when it fits in the label
            if (m.Inputs is { } inputs && expected.Length + InputsKey.Length + 1 + 8 * inputs.Count <= LabelMax)
                expected += "+" + InputsKey + string.Concat(inputs.Select(h => h.Length >= 8 ? h[..8] : "00000000"));
            return m.LobbyLabel == expected && VpnLobby.ValidModsLabel(System.Text.Encoding.ASCII.GetBytes(expected)) ? expected : null;
        }

        /// <summary>A randomized world a lobby label names: (generator number, code text, input-hash check, Nocturne version,
        /// base part), or null when the label names none. The base part is every other key in label order, each a
        /// "nocturne@&lt;version&gt;" (at most one) or "m:" + 8 lowercase hex (a mod by content, <see cref="LoadedHash"/>), at most
        /// 8 and none twice; null for the game alone. Malformed: a world part is there but the label is not one this launcher
        /// reads (more than one world or check, a part it does not know): only "modded" then.</summary>
        public static (int Generator, string Code, string Check, string? Nocturne, string? Base)? WorldOf(string? hostLabel, out bool malformed)
        {
            malformed = false;
            var parts = (hostLabel ?? "").Split('+');
            var worlds = parts.Where(p => p.StartsWith("rando", StringComparison.Ordinal) && p.Contains(':')).ToList();
            if (worlds.Count == 0) return null;
            var checks = parts.Where(p => p.StartsWith("w:", StringComparison.Ordinal)).ToList();
            // "g:" + 8 hex: the generator build (BuildOf), at most one; it is inside w: too, so it only names a mismatch.
            var builds = parts.Where(p => p.StartsWith(BuildKey, StringComparison.Ordinal)).ToList();
            // "i:" + 8 hex per root (InputsOf), at most one; inside w: too, so it only names what differs.
            var inputs = parts.Where(p => p.StartsWith(InputsKey, StringComparison.Ordinal)).ToList();
            var others = parts.Where(p => !worlds.Contains(p) && !checks.Contains(p) && !builds.Contains(p) && !inputs.Contains(p)).ToList();
            var nocturne = NocturneOf(hostLabel);
            static bool ModKey(string p) => p.Length == ModToken.Length + 8 && p.StartsWith(ModToken, StringComparison.Ordinal) &&
                                            p[ModToken.Length..].All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
            var head = worlds[0][..worlds[0].IndexOf(':')];
            if (worlds.Count != 1 || checks.Count != 1 || others.Count > 8 || others.Distinct().Count() != others.Count ||
                others.Count(p => p.StartsWith(LabelKey + "@", StringComparison.Ordinal)) > 1 ||
                others.Any(p => !ModKey(p) && p != LabelKey + "@" + nocturne) ||
                builds.Count > 1 || (builds.Count == 1 && BuildOf(hostLabel) == null) ||
                inputs.Count > 1 || (inputs.Count == 1 && InputsOf(hostLabel) == null) ||
                checks[0].Length != 10 || !checks[0][2..].All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f') ||
                head.Length is < 6 or > 9 || !head[5..].All(char.IsAsciiDigit))
            {
                malformed = true;
                return null;
            }
            return (int.Parse(head[5..]), worlds[0][(head.Length + 1)..], checks[0][2..], nocturne, others.Count > 0 ? string.Join('+', others) : null);
        }

        private const string BuildKey = "g:";
        private const string InputsKey = "i:";
        private const int LabelMax = 255; // VpnLobby.ValidModsLabel; generate.LABEL_MAX

        /// <summary>The input-hash parts a world label names ("i:" + 8 hex per root: the game, then each base mod in load
        /// order; generate.lobby_label), or null when it names none or a malformed one. Untrusted: only compared.</summary>
        public static IReadOnlyList<string>? InputsOf(string? hostLabel)
        {
            var found = (hostLabel ?? "").Split('+').Where(p => p.StartsWith(InputsKey, StringComparison.Ordinal)).ToList();
            if (found.Count != 1) return null;
            var hex = found[0][InputsKey.Length..];
            if (hex.Length == 0 || hex.Length % 8 != 0 || hex.Length > 8 * 16 || !hex.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f')) return null;
            return Enumerable.Range(0, hex.Length / 8).Select(i => hex.Substring(i * 8, 8)).ToList();
        }

        /// <summary>Why a world built here is not the host's, from the label's "i:" against this build's own parts: the game's
        /// files, and the names of the base mods whose copies differ. Null when either side lacks them or they do not line up
        /// (then only "something differs" is known).</summary>
        public static (bool Game, IReadOnlyList<string> Mods)? WhatDiffers(string? hostLabel, Randomizer.WorldManifest made)
        {
            if (InputsOf(hostLabel) is not { } host || made.Inputs is not { } mine || host.Count != mine.Count) return null;
            string Short(string h) => h.Length >= 8 ? h[..8] : "00000000";
            var mods = Enumerable.Range(1, mine.Count - 1).Where(i => host[i] != Short(mine[i])).Select(i => made.Mods[i - 1]).ToList();
            var game = host[0] != Short(mine[0]);
            return game || mods.Count > 0 ? (game, mods) : null;
        }

        /// <summary>The generator build a world label names ("g:" + the first 8 hex of the host generator's build id), or null
        /// when it names none (a world of a generator before build ids, or no world). Untrusted: only compared.</summary>
        public static string? BuildOf(string? hostLabel)
        {
            var found = (hostLabel ?? "").Split('+').Where(p => p.StartsWith(BuildKey, StringComparison.Ordinal)).ToList();
            return found.Count == 1 && found[0].Length == BuildKey.Length + 8 &&
                   found[0][BuildKey.Length..].All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f') ? found[0][BuildKey.Length..] : null;
        }

        /// <summary>True when a generator of build <paramref name="ownBuild"/> (64 hex; null: unknown) would not make the world
        /// this host label names: the label's "g:" is another build, or it has none (a generator before build ids). False for no
        /// label or an unknown build: nothing to tell.</summary>
        public static bool OtherBuild(string? hostLabel, string? ownBuild) =>
            hostLabel != null && ownBuild != null && (BuildOf(hostLabel) is not { } host || !ownBuild.StartsWith(host, StringComparison.Ordinal));

        /// <summary>What the page says when this package's generator is not the host's: it would build another world.</summary>
        public static string OtherBuildText => L.T("This world was made by another randomizer build; both players need the same Nightwatch package.");

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

        /// <summary>What the Workshop tells when it first opens, whatever the Mods choice (user, 4 Nov: advertise Nocturne): Try when
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
            // 0.2.8 hosts add "patches:" + 8 hex (party_vpnlobby.h party_label): compared on its own, the rest is the mods part.
            var parts = (hostLabel ?? "").Trim().Split('+', StringSplitOptions.RemoveEmptyEntries);
            var hostPatches = parts.LastOrDefault(p => p.StartsWith(PatchesKey, StringComparison.Ordinal))?[PatchesKey.Length..];
            var patchesDiffer = hostPatches != null && local.PatchesHash is { } ownPatches &&
                                !string.Equals(hostPatches, ownPatches, StringComparison.OrdinalIgnoreCase);
            var mods = ModsAdvice(ModsPart(hostLabel), local, offered);
            return mods with { PatchesDiffer = patchesDiffer };
        }

        private const string PatchesKey = "patches:";

        /// <summary>A host label without its "patches:" part: what it says about mods ("" none).</summary>
        public static string ModsPart(string? hostLabel) =>
            string.Join('+', (hostLabel ?? "").Trim().Split('+', StringSplitOptions.RemoveEmptyEntries)
                .Where(p => !p.StartsWith(PatchesKey, StringComparison.Ordinal)));

        private static JoinChoice ModsAdvice(string label, LocalMods local, IReadOnlyCollection<string>? offered)
        {
            if (WorldOf(label, out var malformed) is { } world)
            {
                if (world.Generator > Randomizer.GeneratorMajor) return new JoinChoice(JoinKind.WorldNewer);
                var decoded = WorldCode.Decode(world.Code);
                if (decoded.Kind == WorldCodeKind.Newer) return new JoinChoice(JoinKind.WorldNewer);
                if (decoded is not { Kind: WorldCodeKind.Ok, Canonical: { } code } || decoded.Canonical.ToLowerInvariant() != world.Code)
                    return new JoinChoice(JoinKind.Modded);
                // The folder name holds no generator: the world playing here is the host's only when its manifest's generator
                // is the label's too (an older world still plays as built, so a guest already on it may join), and its base
                // mods are the label's by content (the party mods hash covers every file of them, read or not).
                if (local.Mode == "world" && local.World == $"Randomizer {code} {world.Check}" && local.WorldGenerator == world.Generator &&
                    BaseMatches(local.WorldBase, world.Base))
                    return new JoinChoice(JoinKind.Join, world.Nocturne, code, world.Check, Base: world.Base);
                if (world.Generator < Randomizer.GeneratorMajor)
                    return new JoinChoice(JoinKind.WorldOlder, world.Nocturne, code, world.Check, Base: world.Base);
                if (!local.CanBuildWorlds)
                    return new JoinChoice(JoinKind.WorldNotAvailable, world.Nocturne, code, world.Check, Base: world.Base);
                var have = local.BaseTokens?.Split(',', StringSplitOptions.RemoveEmptyEntries).ToHashSet(StringComparer.Ordinal);
                if (world.Nocturne is { } needed && !string.Equals(local.NocturneVersion, needed, StringComparison.OrdinalIgnoreCase) &&
                    offered?.Contains(needed, StringComparer.OrdinalIgnoreCase) != true && have?.Contains(LabelKey + "@" + needed) != true)
                    return new JoinChoice(JoinKind.NocturneNotAvailable, needed);
                // A mod named by content: some folder here (any name) must hold those files; nothing can fetch it.
                var missing = have == null ? new List<string>() : (world.Base ?? "").Split('+', StringSplitOptions.RemoveEmptyEntries)
                    .Where(t => t.StartsWith(ModToken, StringComparison.Ordinal) && !have.Contains(t)).ToList();
                if (missing.Count > 0)
                    return new JoinChoice(JoinKind.WorldModMissing, world.Nocturne, code, world.Check, Base: world.Base, Missing: string.Join(",", missing));
                return new JoinChoice(JoinKind.BuildWorld, world.Nocturne, code, world.Check, Base: world.Base);
            }
            if (malformed) return new JoinChoice(JoinKind.Modded);
            if (NocturneOf(label) is { } version)
            {
                if (local.Mode == "nocturne" && string.Equals(local.NocturneVersion, version, StringComparison.OrdinalIgnoreCase))
                    return new JoinChoice(JoinKind.Join);
                var have = string.Equals(local.NocturneVersion, version, StringComparison.OrdinalIgnoreCase) ||
                           offered?.Contains(version, StringComparer.OrdinalIgnoreCase) == true;
                return new JoinChoice(have ? JoinKind.GetNocturne : JoinKind.NocturneNotAvailable, version);
            }
            // "mods:" + 8 hex is the host's party mod hash (Nocturne with other mods, or any other set): the same hash here
            // means the same gameplay files, whatever the mods are called (user, 4 Nov: Nocturne with another mod said mismatch).
            if (label.StartsWith("mods:", StringComparison.Ordinal) && local.ModsHash is { } own)
                return new JoinChoice(string.Equals(label[5..], own, StringComparison.OrdinalIgnoreCase) ? JoinKind.Join : JoinKind.Modded);
            var modsOn = local.Mode is "nocturne" or "world" || (local.Mode == "my" && local.EnabledCount > 0);
            if (label.Length > 0) return new JoinChoice(local.ModsHash == null && modsOn ? JoinKind.ModdedUnchecked : JoinKind.Modded);
            if (local.ModsHash == NoGameplayMods) return new JoinChoice(JoinKind.Join);  // cosmetic mods only
            return modsOn ? new JoinChoice(JoinKind.MismatchJoinAnyway) : new JoinChoice(JoinKind.Join);
        }

        /// <summary>The guest's side of "Use Nocturne X and join": installs that version from the manifest when the folder holds
        /// another one, then sets Nocturne mode and writes mods.json. Throws with a message the page shows.</summary>
        public static async Task UseNocturneAsync(LauncherSettings s, string modsDir, string version, ModManifest? manifest,
            IProgress<double>? progress, CancellationToken ct = default)
        {
            await EnsureNocturneAsync(modsDir, version, manifest, progress, ct);
            s.Set(ProfileKey, "nocturne");
            Apply(s, modsDir);
        }

        /// <summary>The local side for JoinAdvice: mode, installed Nocturne version (any known one), My mods' count, the party mods
        /// and patches hashes as a start would have them now, the world world mode plays, whether worlds can be built, that
        /// world's generator and base by content, and (with <paramref name="baseTokens"/>, when a host's world names a base mod
        /// by content) the keys this PC's mods stand for, worked out in the background.</summary>
        public static LocalMods Local(LauncherSettings s, string modsDir, ModManifest? manifest, bool baseTokens = false)
        {
            var world = Mode(s) == "world" ? s.Str(Randomizer.WorldKey) : null;
            var made = world != null && Randomizer.Parse(world) != null ? Randomizer.ReadManifest(Path.Combine(modsDir, world)) : null;
            var worldBase = made != null ? SafeBaseKeys(modsDir, made.Mods) : null;
            return new(Mode(s), Installed(modsDir, manifest) is { Kind: InstalledKind.Known } i ? i.Version : null, MyModsCount(s, modsDir),
                PartyHashes.Current.Mods, PartyHashes.Current.Patches, world, Randomizer.Supported, made?.Generator,
                worldBase, baseTokens ? KnownBaseTokens(modsDir) : null);
        }

        /// <summary>The party hash of a start with no gameplay mods (bb-start writes 64 zeros), first 8 hex.</summary>
        public const string NoGameplayMods = "00000000";

        // Installs this Nocturne version from the manifest unless it is the one in the folder.
        private static async Task EnsureNocturneAsync(string modsDir, string version, ModManifest? manifest, IProgress<double>? progress, CancellationToken ct)
        {
            var installed = await Task.Run(() => Installed(modsDir, manifest), ct);
            if (installed.Kind == InstalledKind.Known && string.Equals(installed.Version, version, StringComparison.OrdinalIgnoreCase)) return;
            if (manifest?.Find(version) is not { } entry)
                throw new InvalidDataException(L.F($"Nocturne {version} is not available for download."));
            await InstallAsync(entry, modsDir, progress, ct);
        }

        /// <summary>The guest's side of "Build world and join" (a BuildWorld choice): reuses the world when this folder already
        /// has it (<see cref="HasWorld"/>), else finds the base mods the label names by content in folders of any name, gets the
        /// host's Nocturne version when none here has it (any other missing mod: a message naming its key), and builds the
        /// world from this player's game files on them; then plays it (world mode). A build whose input hash is not the host's check is
        /// refused (another game dump or Nocturne copy would make another world) and the new folder removed. Throws with a
        /// message the page shows; <paramref name="building"/> is told when the generator starts. With <paramref name="hostLabel"/>
        /// (the host's lobby label), a world that must be built is refused before the build when this package's generator build
        /// is not the label's "g:" (or the label has none: a generator before build ids), and a build that does not match says
        /// so: another generator build makes another world from the same code and files. <paramref name="buildProgress"/> gets the
        /// build's fraction done and step text as the generator reports them (<see cref="Randomizer.Generate"/>).</summary>
        public static async Task UseWorldAsync(LauncherSettings s, string modsDir, JoinChoice choice, ModManifest? manifest,
            IProgress<double>? progress, Action? building = null, CancellationToken ct = default, string? hostLabel = null,
            Action<double, string>? buildProgress = null)
        {
            if (choice is not { Kind: JoinKind.BuildWorld or JoinKind.Join, Code: { } code, Check: { } check })
                throw new InvalidOperationException(L.T("This host's world can't be built here."));
            // The base by content (user, 5 Nov): each key of the label is a folder here under any name; only a missing Nocturne
            // version is fetched (NameFolder first gives the launcher's install its version name).
            if (choice.Version != null) NameFolder(s, modsDir);
            var want = $"Randomizer {code} {check}";
            if (!await Task.Run(() => HasWorld(modsDir, code, check, choice.Base), ct))
            {
                if (hostLabel != null && OtherBuild(hostLabel, await Randomizer.BuildIdAsync(ct)))
                    throw new InvalidDataException(OtherBuildText);
                var (baseMods, missing) = await Task.Run(() => ResolveBase(modsDir, choice.Base, manifest), ct);
                if (baseMods == null && choice.Version is { } version && missing.Contains(LabelKey + "@" + version.ToLowerInvariant()))
                {
                    await EnsureNocturneAsync(modsDir, version, manifest, progress, ct);
                    NameFolder(s, modsDir);
                    (baseMods, missing) = await Task.Run(() => ResolveBase(modsDir, choice.Base, manifest), ct);
                }
                if (baseMods == null) throw new InvalidDataException(MissingModsText(missing));
                building?.Invoke();
                var existed = new HashSet<string>(Mods.Discover(modsDir), StringComparer.OrdinalIgnoreCase);
                var name = await Randomizer.Generate(s, code, baseMods, buildProgress, ct);
                void RemoveNew()
                {
                    if (!existed.Contains(name))
                        try { Directory.Delete(Path.Combine(modsDir, name), recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
                }
                // A package whose generator is not the launcher's makes another world under any name: say so, not "your files".
                if (Randomizer.ReadManifest(Path.Combine(modsDir, name)) is not { } made || made.Generator != Randomizer.GeneratorMajor)
                {
                    RemoveNew();
                    throw new InvalidDataException(L.T("This package's randomizer does not match the launcher. Reinstall Nightwatch Co-op."));
                }
                if (name != want)
                {
                    var kept = KeepMismatch(Path.Combine(modsDir, name), hostLabel);
                    RemoveNew();
                    if (OtherBuild(hostLabel, made.GeneratorBuild)) throw new InvalidDataException(OtherBuildText);
                    throw new InvalidDataException(MismatchText(WhatDiffers(hostLabel, made), choice.Version != null) +
                        (kept != null ? " " + L.F($"The details are in {kept}.") : ""));
                }
            }
            Randomizer.Use(s, want);
            Apply(s, modsDir);
        }

        /// <summary>What the page says when the world built here is not the host's: what differs when the label says
        /// (<see cref="WhatDiffers"/>), else that the game's or the base mods' files do.</summary>
        public static string MismatchText((bool Game, IReadOnlyList<string> Mods)? differs, bool nocturne)
        {
            if (differs is not { } d)
                return nocturne
                    ? L.T("Your game files or your Nocturne files differ from the host's, so this world would not match theirs. Check that your game is version 1.09 and Nocturne came from the launcher.")
                    : L.T("Your game files differ from the host's, so this world would not match theirs. Check that your game is version 1.09.");
            var mods = string.Join(", ", d.Mods);
            if (d.Game && d.Mods.Count > 0)
                return L.F($"Your game files and your copy of {mods} differ from the host's, so this world would not match theirs.");
            if (d.Game)
                return L.T("Your game files differ from the host's (your mods match theirs), so this world would not match theirs. Your game folder holds changed files: another copy of the game, or a mod once copied into it.");
            return L.F($"Your copy of {mods} differs from the host's (your game files match theirs), so this world would not match theirs. Remove it from your mods folder and join again: the launcher downloads Nocturne again; another mod, get it from the host.");
        }

        /// <summary>A failed join build's manifest (every file it read, with its hash) and the host's label, kept as
        /// logs\world-mismatch.json before the build is removed, so the file that differs can be found later by comparing it
        /// with the host's manifest. Returns that path, or null when it could not be written.</summary>
        private static string? KeepMismatch(string builtFolder, string? hostLabel)
        {
            try
            {
                var target = Path.Combine(Path.GetDirectoryName(GameRunner.SessionLog)!, "world-mismatch.json");
                var manifest = JsonNode.Parse(File.ReadAllText(Path.Combine(builtFolder, "randomizer_manifest.json")));
                var kept = new JsonObject { ["host_label"] = hostLabel, ["built"] = Path.GetFileName(builtFolder), ["manifest"] = manifest };
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.WriteAllText(target, kept.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
                return target;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
            {
                return null;
            }
        }

        /// <summary>What the page says when a world's base mods are not here: their keys as the label names them ("m:" + 8 hex
        /// of the files, or a Nocturne version), so the host can tell which mods they are.</summary>
        public static string MissingModsText(IEnumerable<string> keys)
        {
            var list = keys.Select(k => k.StartsWith(LabelKey + "@", StringComparison.Ordinal) ? "Nocturne " + k[(LabelKey.Length + 1)..] : k);
            return string.Format(L.T("This host's world is built on mods you don't have ({0}). Ask the host which mods they are and add the same files to your mods folder (any folder name works)."),
                string.Join(", ", list));
        }

        /// <summary>This folder already holds the host's world: "Randomizer CODE CHECK" whose manifest is this generator's, with
        /// that code and input hash, built on base mods that a label names <paramref name="basePart"/> now (by content, so a
        /// base under another folder name counts; null: the game alone). A folder of that name from another generator is
        /// another world (the name holds no generator): it is built again.</summary>
        public static bool HasWorld(string modsDir, string code, string check, string? basePart) =>
            Randomizer.ReadManifest(Path.Combine(modsDir, $"Randomizer {code} {check}")) is { } m && m.Generator == Randomizer.GeneratorMajor &&
            m.Code == code && m.InputHash.StartsWith(check, StringComparison.Ordinal) && BaseMatches(SafeBaseKeys(modsDir, m.Mods), basePart);

        // BaseKeysOf, or "?" (matches no base) when a folder is missing or can't be read.
        private static string? SafeBaseKeys(string modsDir, IReadOnlyList<string> baseMods)
        {
            try { return BaseKeysOf(modsDir, baseMods); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { return "?"; }
        }
    }
}
