using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Each randomized world's own saves. While a world is played, the game's user folder (BB_USER_DIR: savedata,
    /// savedata_party, save_backups, trophies, party files) is &lt;profile&gt;\worlds\&lt;folder&gt;, for a host and a guest alike,
    /// so the engine's save rules (a party session plays savedata_party, a solo game savedata) hold inside it unchanged.
    /// world.json in that folder holds the world's code and check (the folder name's 8 hex, which covers the generator build)
    /// and its name. The name is this PC's only: it is never in the world's files, its folder name, the lobby label or a hash,
    /// and renaming moves nothing. The folder name comes from the name the world had when its folder was made.
    /// Saves of earlier launchers in &lt;profile&gt;\randomizer\&lt;code&gt; &lt;check&gt; are copied here (copied, checked file by file,
    /// then used); the old folder is left as it was, as a backup.
    /// </summary>
    public static class WorldSaves
    {
        public const string RootName = "worlds";
        public const string InfoFile = "world.json";
        public const string LegacyRootName = "randomizer";
        public const int MaxName = 48;
        private const string Staging = ".copying";
        private const int MaxFolder = 40;

        /// <summary>One world's saves. Named: the player chose the name (a default name gives way to a guest's default).
        /// Joined: a copy made to join a host. Legacy: an earlier launcher's folder not copied yet (its name is its code).</summary>
        public sealed record Entry(string Folder, string Code, string Check, string Name, bool Named = false, bool Joined = false,
            string? GeneratorBuild = null, bool Legacy = false);

        /// <summary>What happened to one old save folder: To is the new folder (null: not moved, Error says why).</summary>
        public sealed record Moved(string From, string? To, string? Error);

        private static readonly object Lock = new();
        // Copies wait for each other and a game start waits for them (MigrateWorld): never two copies of one folder.
        private static readonly object MigrationLock = new();
        private static readonly Dictionary<string, string> Found = new(StringComparer.OrdinalIgnoreCase);

        /// <summary>Tests only: runs on the finished copy before it is checked (a damaged copy must leave the old folder as it was).</summary>
        internal static Action<string>? AfterCopy;

        public static string WorldsDir(string profileFolder) => Path.Combine(profileFolder, RootName);

        /// <summary>An earlier launcher's save folder for this world: &lt;profile&gt;\randomizer\&lt;code&gt; &lt;check&gt;.</summary>
        public static string LegacyFolder(string profileFolder, Randomizer.World world) => Randomizer.SaveFolder(profileFolder, world);

        // ---------------------------------------------------------------- the info file

        public static Entry? Read(string folder)
        {
            try
            {
                if (JsonNode.Parse(File.ReadAllText(Path.Combine(folder, InfoFile))) is not JsonObject o) return null;
                string? Str(string key) => o[key] is JsonValue v && v.TryGetValue<string>(out var x) ? x : null;
                bool Bool(string key) => o[key] is JsonValue v && v.TryGetValue<bool>(out var x) && x;
                if (Str("code") is not { Length: > 0 } code || Str("check") is not { Length: 8 } check) return null;
                var name = CleanName(Str("name") ?? "");
                return new Entry(folder, code, check, name.Length > 0 ? name : code, Bool("named"), Bool("joined"), Str("generator_build"));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException)
            {
                return null;
            }
        }

        /// <summary>Writes world.json beside it and moves it into place, so a stopped write never leaves half a file.</summary>
        public static void Write(Entry e)
        {
            var o = new JsonObject
            {
                ["code"] = e.Code,
                ["check"] = e.Check,
                ["name"] = e.Name,
                ["named"] = e.Named,
                ["joined"] = e.Joined,
            };
            if (e.GeneratorBuild != null) o["generator_build"] = e.GeneratorBuild;
            Directory.CreateDirectory(e.Folder);
            var path = Path.Combine(e.Folder, InfoFile);
            File.WriteAllText(path + ".tmp", o.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
            File.Move(path + ".tmp", path, overwrite: true);
            lock (Lock) Found[Key(Path.GetDirectoryName(Path.GetDirectoryName(e.Folder)!)!, e.Code, e.Check)] = e.Folder;
        }

        // ---------------------------------------------------------------- names

        /// <summary>A name as kept: trimmed, control characters out, at most MaxName characters.</summary>
        public static string CleanName(string name)
        {
            var text = new string(name.Where(c => !char.IsControl(c)).ToArray()).Trim();
            return text.Length > MaxName ? text[..MaxName].TrimEnd() : text;
        }

        /// <summary>A usable world name, or null with the reason.</summary>
        public static string? ValidName(string name, out string error)
        {
            error = "";
            var clean = CleanName(name);
            if (clean.Length == 0) { error = L.T("Enter a name for the world."); return null; }
            if (name.Trim().Length > MaxName) { error = L.F($"Use {MaxName} characters or fewer."); return null; }
            return clean;
        }

        /// <summary>"World N": the first number no world of this profile has.</summary>
        public static string DefaultName(string profileFolder)
        {
            var taken = new HashSet<string>(List(profileFolder).Select(e => e.Name), StringComparer.OrdinalIgnoreCase);
            for (var n = 1; ; n++)
            {
                var name = L.F($"World {n}");
                if (!taken.Contains(name)) return name;
            }
        }

        /// <summary>A guest's copy of a host's world: "&lt;host's lobby name&gt;'s world".</summary>
        public static string GuestName(string host)
        {
            var clean = CleanName(host);
            var name = L.F($"{(clean.Length > 0 ? clean : L.T("Host"))}'s world");
            return CleanName(name);
        }

        /// <summary>A folder name for this name under worlds\: ASCII letters (accents dropped), digits, spaces, - and _, at most 40
        /// characters, never a Windows device name, and not taken yet (" 2", " 3" ... when it is). ASCII only: the folder is the
        /// game's BB_USER_DIR, which the engine's C side opens with narrow (code page) paths.</summary>
        public static string SafeFolder(string worldsDir, string name)
        {
            var plain = new string(name.Normalize(System.Text.NormalizationForm.FormD)
                .Where(c => System.Globalization.CharUnicodeInfo.GetUnicodeCategory(c) != System.Globalization.UnicodeCategory.NonSpacingMark).ToArray());
            var text = new string(plain.Select(c => c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or ' ' or '-' or '_' ? c : ' ').ToArray());
            text = string.Join(' ', text.Split(' ', StringSplitOptions.RemoveEmptyEntries));
            if (text.Length > MaxFolder) text = text[..MaxFolder].TrimEnd();
            if (text.Length == 0) text = "World";
            var device = text.Split(' ')[0].ToUpperInvariant();
            if (device is "CON" or "PRN" or "AUX" or "NUL" || (device.Length == 4 && (device.StartsWith("COM") || device.StartsWith("LPT")) && char.IsDigit(device[3])))
                text = "World " + text;
            var folder = text;
            for (var n = 2; Directory.Exists(Path.Combine(worldsDir, folder)) || Directory.Exists(Path.Combine(worldsDir, folder + Staging)) ||
                            File.Exists(Path.Combine(worldsDir, folder)); n++)
                folder = text + " " + n;
            return folder;
        }

        // ---------------------------------------------------------------- finding a world's folder

        private static string Key(string profileFolder, string code, string check) =>
            Path.GetFullPath(profileFolder).TrimEnd('\\', '/') + "|" + code + "|" + check;

        /// <summary>This profile's worlds, by name: the folders under worlds\ with a world.json, and (legacy) the earlier
        /// launcher's folders not copied yet.</summary>
        public static List<Entry> List(string profileFolder, bool legacy = true)
        {
            var list = new List<Entry>();
            var root = WorldsDir(profileFolder);
            try
            {
                if (Directory.Exists(root))
                    foreach (var folder in Directory.EnumerateDirectories(root))
                        if (!folder.EndsWith(Staging, StringComparison.OrdinalIgnoreCase) && Read(folder) is { } e)
                            list.Add(e);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            if (legacy)
                foreach (var (folder, world) in LegacyFolders(profileFolder))
                    if (!list.Any(e => e.Code == world.Code && e.Check == world.Hash))
                        list.Add(new Entry(folder, world.Code, world.Hash, world.Code, Legacy: true));
            return list.OrderBy(e => e.Name, StringComparer.CurrentCultureIgnoreCase).ThenBy(e => e.Folder, StringComparer.OrdinalIgnoreCase).ToList();
        }

        /// <summary>The earlier launcher's per-world folders of this profile (randomizer\&lt;code&gt; &lt;check&gt;).</summary>
        public static List<(string Folder, Randomizer.World World)> LegacyFolders(string profileFolder)
        {
            var list = new List<(string, Randomizer.World)>();
            var root = Path.Combine(profileFolder, LegacyRootName);
            try
            {
                if (Directory.Exists(root))
                    foreach (var folder in Directory.EnumerateDirectories(root))
                        if (Randomizer.Parse("Randomizer " + Path.GetFileName(folder)) is { } world)
                            list.Add((folder, world));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            return list;
        }

        /// <summary>This world's folder under worlds\, or null.</summary>
        public static Entry? Find(string profileFolder, string code, string check)
        {
            var key = Key(profileFolder, code, check);
            lock (Lock)
                if (Found.TryGetValue(key, out var known) && Read(known) is { } cached && cached.Code == code && cached.Check == check)
                    return cached;
            var found = List(profileFolder, legacy: false).FirstOrDefault(e => e.Code == code && e.Check == check);
            lock (Lock)
            {
                if (found != null) Found[key] = found.Folder;
                else Found.Remove(key);
            }
            return found;
        }

        /// <summary>The user folder the game plays this world in: its folder under worlds\; else an earlier launcher's folder that
        /// has not been copied yet (it keeps working until MigrateWorld copies it); else a new folder with a default name.</summary>
        public static string Folder(string profileFolder, Randomizer.World world, string? modsDir = null)
        {
            if (Find(profileFolder, world.Code, world.Hash) is { } entry) return entry.Folder;
            var legacy = LegacyFolder(profileFolder, world);
            if (Directory.Exists(legacy)) return legacy;
            try
            {
                return Create(profileFolder, world, DefaultName(profileFolder), named: false, joined: false, BuildOf(modsDir, world)).Folder;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // The saves folder can't be written: the engine reports it when it starts.
                return Path.Combine(WorldsDir(profileFolder), SafeFolder(WorldsDir(profileFolder), world.Code));
            }
        }

        private static string? BuildOf(string? modsDir, Randomizer.World world) =>
            modsDir == null ? null : Randomizer.ReadManifest(Path.Combine(modsDir, world.Folder))?.GeneratorBuild;

        /// <summary>Makes this world's folder with world.json (the world must not have one yet).</summary>
        public static Entry Create(string profileFolder, Randomizer.World world, string name, bool named, bool joined, string? generatorBuild = null)
        {
            lock (Lock)
            {
                if (Find(profileFolder, world.Code, world.Hash) is { } existing) return existing;
                var root = WorldsDir(profileFolder);
                Directory.CreateDirectory(root);
                var clean = CleanName(name);
                if (clean.Length == 0) clean = world.Code;
                var entry = new Entry(Path.Combine(root, SafeFolder(root, clean)), world.Code, world.Hash, clean, named, joined, generatorBuild);
                Write(entry);
                return entry;
            }
        }

        /// <summary>True when this world has no old folder left to copy: copied now when <paramref name="mayCopy"/> (no game
        /// runs), else false. A name never goes into a new folder while the old one still holds the saves: the new folder would
        /// be found first and the old saves would stop being played.</summary>
        private static bool OldFolderCopied(string profileFolder, Randomizer.World world, bool mayCopy)
        {
            if (Find(profileFolder, world.Code, world.Hash) != null || !Directory.Exists(LegacyFolder(profileFolder, world))) return true;
            return mayCopy && MigrateWorld(profileFolder, world).To != null;
        }

        /// <summary>The host's name for a world it builds (or a rename): its folder is made when it has none; renaming moves
        /// nothing. An earlier launcher's folder of this world is copied first (<paramref name="mayCopy"/>: no game runs); an
        /// IOException when it can't be.</summary>
        public static Entry SetName(string profileFolder, Randomizer.World world, string name, string? modsDir = null, bool mayCopy = true)
        {
            var clean = CleanName(name);
            if (clean.Length == 0) throw new ArgumentException("empty world name", nameof(name));
            if (!OldFolderCopied(profileFolder, world, mayCopy))
                throw new IOException(L.T("This world's saves are still in their old folder. Close the game and try again."));
            lock (Lock)
            {
                if (Find(profileFolder, world.Code, world.Hash) is not { } entry)
                    return Create(profileFolder, world, clean, named: true, joined: false, BuildOf(modsDir, world));
                var renamed = entry with { Name = clean, Named = true };
                Write(renamed);
                return renamed;
            }
        }

        /// <summary>A guest's copy: "&lt;host&gt;'s world" unless the player named this world before (a default name gives way).
        /// An earlier launcher's folder of this world is copied first (outside Lock: a copy takes MigrationLock, then Lock), so the
        /// name lands in the folder the game will use; when it can't be, the old folder stays in use, unnamed.</summary>
        public static Entry NameForGuest(string profileFolder, Randomizer.World world, string hostName, string? modsDir = null, bool mayCopy = true)
        {
            var name = GuestName(hostName);
            if (!OldFolderCopied(profileFolder, world, mayCopy))
                return new Entry(LegacyFolder(profileFolder, world), world.Code, world.Hash, world.Code, Legacy: true);
            lock (Lock)
            {
                if (Find(profileFolder, world.Code, world.Hash) is not { } entry)
                    return Create(profileFolder, world, name, named: false, joined: true, BuildOf(modsDir, world));
                if (entry.Named) return entry;
                var renamed = entry with { Name = name, Joined = true };
                Write(renamed);
                return renamed;
            }
        }

        /// <summary>After a guest switched to a host's world (CuratedMods.UseWorldAsync): its saves here are named after the
        /// host's lobby name, unless the player named them. Never throws: a name is a nicety, the join goes on.</summary>
        public static void NameForGuest(LauncherSettings s, string hostName)
        {
            try
            {
                if (Randomizer.Active(s) is { } world)
                    NameForGuest(PlayerProfiles.Folder(s, PlayerProfiles.Current(s)), world, hostName, GameEnvironment.ModsDir(s),
                        mayCopy: !Workspace.Current.Game.Running);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { }
        }

        // ---------------------------------------------------------------- moving earlier launchers' saves

        /// <summary>Copies every old per-world folder of these profiles that has no folder under worlds\ yet. The caller makes
        /// sure no game runs. Never throws; each result says what happened.</summary>
        public static List<Moved> MigrateAll(IEnumerable<string> profileFolders)
        {
            var results = new List<Moved>();
            foreach (var profile in profileFolders)
                foreach (var (_, world) in LegacyFolders(profile))
                    if (Find(profile, world.Code, world.Hash) == null)
                        results.Add(MigrateWorld(profile, world));
            return results;
        }

        /// <summary>Copies one old per-world folder to worlds\: into "&lt;folder&gt;.copying" (the game's shader cache left out:
        /// the engine fills it again at its start), checks every file's size and SHA-256 against the old one, writes world.json,
        /// then renames the copy into place; from then on the game uses it. The old folder is only read, and stays as a backup
        /// (randomizer\README.txt says so). A failed copy or check removes only the unfinished copy. Never throws.</summary>
        public static Moved MigrateWorld(string profileFolder, Randomizer.World world)
        {
            var from = LegacyFolder(profileFolder, world);
            lock (MigrationLock)
            {
                if (Find(profileFolder, world.Code, world.Hash) is { } done) return new Moved(from, done.Folder, null);
                if (!Directory.Exists(from)) return new Moved(from, null, "no old folder");
                string? staging = null;
                try
                {
                    var root = WorldsDir(profileFolder);
                    Directory.CreateDirectory(root);
                    // Unfinished copies of this world from a stopped run: copies only, the old folder is intact.
                    foreach (var old in Directory.EnumerateDirectories(root, "*" + Staging))
                        if (Read(old) is { } e && e.Code == world.Code && e.Check == world.Hash)
                            Directory.Delete(old, recursive: true);
                    string folder;
                    lock (Lock)
                    {
                        var name = DefaultName(profileFolder);
                        folder = SafeFolder(root, name);
                        staging = Path.Combine(root, folder + Staging);
                        // world.json first: an interrupted copy is recognised above and cleaned up.
                        WriteStaging(new Entry(staging, world.Code, world.Hash, name));
                    }
                    CopyTree(from, staging);
                    AfterCopy?.Invoke(staging);
                    if (Difference(from, staging) is { } difference)
                        throw new IOException("the copy differs from the old folder: " + difference);
                    var target = Path.Combine(root, folder);
                    Directory.Move(staging, target);
                    staging = null;
                    lock (Lock) Found[Key(profileFolder, world.Code, world.Hash)] = target;
                    WriteLegacyNote(profileFolder);
                    return new Moved(from, target, null);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
                {
                    if (staging != null)
                        try { Directory.Delete(staging, recursive: true); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
                    return new Moved(from, null, e.Message);
                }
            }
        }

        private static void WriteStaging(Entry e)
        {
            Directory.CreateDirectory(e.Folder);
            var o = new JsonObject { ["code"] = e.Code, ["check"] = e.Check, ["name"] = e.Name, ["named"] = false, ["joined"] = false };
            File.WriteAllText(Path.Combine(e.Folder, InfoFile), o.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        }

        // The shader cache (BB_GPU_USER_DIR\cache) is the engine's to fill again (start\main.cpp SeedGpuCache); the save
        // editor's icon cutouts in it too. Everything else is copied.
        private static bool Skipped(string relative) =>
            relative.Equals("cache", StringComparison.OrdinalIgnoreCase) || relative.StartsWith("cache" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);

        private static IEnumerable<string> Files(string root) =>
            Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
                .Select(f => Path.GetRelativePath(root, f))
                .Where(r => !Skipped(r));

        private static void CopyTree(string from, string to)
        {
            foreach (var dir in Directory.EnumerateDirectories(from, "*", SearchOption.AllDirectories))
            {
                var relative = Path.GetRelativePath(from, dir);
                if (!Skipped(relative)) Directory.CreateDirectory(Path.Combine(to, relative));
            }
            foreach (var relative in Files(from))
            {
                if (relative.Equals(InfoFile, StringComparison.OrdinalIgnoreCase)) continue;
                var target = Path.Combine(to, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.Copy(Path.Combine(from, relative), target, overwrite: false);
            }
        }

        /// <summary>The first file whose presence, size or content differs between the old folder and its copy (world.json and
        /// the shader cache aside), or null when they hold the same files.</summary>
        internal static string? Difference(string from, string copy)
        {
            var a = Files(from).Where(r => !r.Equals(InfoFile, StringComparison.OrdinalIgnoreCase)).OrderBy(r => r, StringComparer.OrdinalIgnoreCase).ToList();
            var b = Files(copy).Where(r => !r.Equals(InfoFile, StringComparison.OrdinalIgnoreCase)).OrderBy(r => r, StringComparer.OrdinalIgnoreCase).ToList();
            if (a.Count != b.Count || !a.SequenceEqual(b, StringComparer.OrdinalIgnoreCase))
                return a.Except(b, StringComparer.OrdinalIgnoreCase).Concat(b.Except(a, StringComparer.OrdinalIgnoreCase)).FirstOrDefault() ?? "file list";
            foreach (var relative in a)
            {
                var (x, y) = (new FileInfo(Path.Combine(from, relative)), new FileInfo(Path.Combine(copy, relative)));
                if (x.Length != y.Length || !Hash(x.FullName).SequenceEqual(Hash(y.FullName))) return relative;
            }
            return null;
        }

        private static byte[] Hash(string path)
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            return SHA256.HashData(stream);
        }

        private static void WriteLegacyNote(string profileFolder)
        {
            try
            {
                File.WriteAllText(Path.Combine(profileFolder, LegacyRootName, "README.txt"),
                    "These folders hold the randomized worlds' saves as earlier launchers kept them.\r\n" +
                    "The launcher copied each one to the \"worlds\" folder beside this one (checked file by file) and uses those copies now;\r\n" +
                    "each copy's world.json names its world. Nothing here is used any more: it is kept as a backup.\r\n" +
                    "Delete it once your worlds' characters load fine.\r\n");
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
    }
}
