using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Randomized worlds. The package's generator (randomizer\) reads the player's own game and writes a
    /// normal mod folder, "Randomizer ID HASH" (<see cref="FolderName"/>), kept last in the load order. The world's code is
    /// in its randomizer_manifest.json, never in the folder name: a code grows with its lists, and a long one in the name
    /// put the world's files past Windows' 259-character paths. Each world has its own saves (<see cref="WorldSaves"/>).
    /// </summary>
    public static class Randomizer
    {
        public sealed record World(string Folder, string Code, string Hash);

        /// <summary>A world's randomizer_manifest.json, as far as the launcher uses it. LobbyLabel is what a host announces
        /// (base part + "rando&lt;N&gt;:" + code + "+w:" + the first 8 hex of InputHash + "+g:" + the first 8 of GeneratorBuild);
        /// BaseLabel the launcher's --base-label. GeneratorBuild: the generator's own build id (64 hex, tools/randomizer/buildid.py;
        /// null for worlds of a generator that did not record one), which InputHash also covers. Inputs: InputHash's parts by
        /// where the files came from (game_input, then mod_inputs' hashes in load order; 64 hex, or "" for a root nothing was
        /// read from), null for worlds of a generator that did not record them; the label's "+i:" carries their first 8 hex.</summary>
        public sealed record WorldManifest(string Code, int Generator, string InputHash, IReadOnlyList<string> Mods, string? BaseLabel, string? LobbyLabel,
            string? GeneratorBuild = null, IReadOnlyList<string>? Inputs = null);

        /// <summary>seedcode.GENERATOR_MAJOR: the output version, the label's "rando&lt;N&gt;"; another number builds another world.</summary>
        public const int GeneratorMajor = 2;

        /// <summary>The setting naming the world that game mode "world" plays (a folder name in the mods folder).</summary>
        public const string WorldKey = "world_folder";
        private const string ReturnKey = "world_return_mode";

        public sealed record Options(ulong Seed, int Chance, string Scaling, string Rewards, IReadOnlyList<int> Exclude, bool Bosses = false, bool Treasure = false, bool Shops = false, bool Drops = false, bool TreasureByType = false, bool Starters = false, int Headroom = 100, bool ChaliceMaterials = false, int StarterMelee = 3, int StarterGuns = 2,
            bool WitchMix = false, bool ShadowsMix = false, bool EmissaryMix = false, bool FailuresMix = false,
            string BellMaidens = "off", bool ChaliceBosses = false, bool HardRom = false, bool HardAmygdala = false, bool HardEbrietas = false,
            bool DifficultEnemies = false, bool SecondStage = false, bool AwayFromHome = false, bool FreePoses = false, bool StarterArmor = false,
            bool ShadowsSummons = false, bool ChaliceEnemies = false, bool BossesAsEnemies = false, bool CoiledHost = false, bool GemRoulette = false,
            bool CursedGems = false, int BossEnemyChance = 25, int BossEnemyMax = 1, bool HugeBossesAsEnemies = false,
            int BossEnemyHp = 8, bool NoWinterLanterns = false, IReadOnlyList<int>? IncludeOnly = null, bool ScorchedBeast = false);

        /// <summary>The bell maidens' choices (seedcode option bell_maidens 0-2).</summary>
        public static readonly string[] BellMaidenChoices = { "off", "normal", "difficult" };

        // "Randomizer <id> <check>". The id is FolderName's; a world made before it has its code there, and plays on.
        private static readonly Regex WorldName = new(@"^Randomizer [0-9A-Za-z-]+ ([0-9a-f]{8})$", RegexOptions.CultureInvariant);
        private static readonly Dictionary<string, (DateTime Written, World? World)> Loaded = new(StringComparer.OrdinalIgnoreCase);

        public static string Generator => Path.Combine(Paths.PortDir, "randomizer", "randomizer.exe");

        /// <summary>This package has a generator (the page stays hidden without one).</summary>
        public static bool Supported => File.Exists(Generator);

        /// <summary>The folder the generator writes this world to (generate.folder_name): "Randomizer", the first 12 hex of
        /// the code's SHA-256, and the check (the first 8 hex of the input hash). 32 characters for any code.</summary>
        public static string FolderName(string code, string check) =>
            $"Randomizer {Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(Encoding.ASCII.GetBytes(code)))[..12].ToLowerInvariant()} {check}";

        /// <summary>A world's folder by its name alone (nothing is read).</summary>
        public static bool IsWorld(string folderName) => WorldName.IsMatch(folderName);

        /// <summary>The world in this folder of the mods folder: its code and check from its manifest, which must agree with
        /// the name's check. Null for any other folder. Kept until the manifest changes (the pages ask often).</summary>
        public static World? Load(string modsDir, string folderName)
        {
            var match = WorldName.Match(folderName);
            if (!match.Success) return null;
            var folder = Path.Combine(modsDir, folderName);
            DateTime written;
            try { written = File.GetLastWriteTimeUtc(Path.Combine(folder, "randomizer_manifest.json")); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
            lock (Loaded)
                if (Loaded.TryGetValue(folder, out var known) && known.Written == written) return known.World;
            var check = match.Groups[1].Value;
            var world = ReadManifest(folder) is { } m && m.InputHash.StartsWith(check, StringComparison.Ordinal)
                ? new World(folderName, m.Code, check) : null;
            lock (Loaded) Loaded[folder] = (written, world);
            return world;
        }

        /// <summary>The world the game would load now: game mode "world"'s world, else in My mods the last enabled one; none
        /// while mods are off for this start.</summary>
        public static World? Active(LauncherSettings s)
        {
            if (s.Bool("safe_mode_next")) return null;
            var mode = CuratedMods.Mode(s);
            if (mode == "world")
                return Load(GameEnvironment.ModsDir(s), s.Str(WorldKey));
            if (mode != "my" || !s.Bool("mods_enabled")) return null;
            var dir = GameEnvironment.ModsDir(s);
            return Mods.Load(dir).Where(m => m.Enabled).Select(m => Load(dir, m.Name)).LastOrDefault(w => w != null);
        }

        /// <summary>Plays this world: game mode "world" loads it and the mods it was built on, nothing else, and keeps My mods'
        /// list as the other modes do; <see cref="StopUsing"/> brings the mode before back. Settings change in memory: the
        /// caller saves them and runs CuratedMods.Apply.</summary>
        public static void Use(LauncherSettings s, string worldFolderName)
        {
            if (CuratedMods.Mode(s) != "world") s.Set(ReturnKey, CuratedMods.Mode(s));
            s.Set(WorldKey, worldFolderName);
            s.Set(CuratedMods.ProfileKey, "world");
        }

        public static void StopUsing(LauncherSettings s)
        {
            if (CuratedMods.Mode(s) != "world") return;
            s.Set(CuratedMods.ProfileKey, s.Str(ReturnKey) is "standard" or "nocturne" ? s.Str(ReturnKey) : "my");
        }

        public static WorldManifest? ReadManifest(string worldFolder)
        {
            try
            {
                if (JsonNode.Parse(File.ReadAllText(Path.Combine(worldFolder, "randomizer_manifest.json"))) is not JsonObject m) return null;
                string? Str(string key) => m[key] is JsonValue v && v.TryGetValue<string>(out var x) ? x : null;
                if (Str("code") is not { } code || Str("input_hash") is not { Length: 64 } hash ||
                    m["generator"] is not JsonValue g || !g.TryGetValue<int>(out var generator))
                    return null;
                var mods = m["mods"] is JsonArray a ? a.Select(n => n is JsonValue v && v.TryGetValue<string>(out var x) ? x : "").ToList() : new List<string>();
                static string Hex(JsonNode? n) => n is JsonValue v && v.TryGetValue<string>(out var x) && x.Length is 0 or 64 ? x : "?";
                List<string>? inputs = null;
                if (m["game_input"] is { } game && m["mod_inputs"] is JsonArray parts)
                {
                    inputs = new List<string> { Hex(game) };
                    inputs.AddRange(parts.Select(p => Hex(p?["hash"])));
                    if (inputs.Contains("?") || inputs.Count != mods.Count + 1) inputs = null;
                }
                return new WorldManifest(code, generator, hash, mods, Str("base_label"), Str("lobby_label"),
                    Str("generator_build") is { Length: 64 } build ? build : null, inputs);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
            {
                return null;
            }
        }

        /// <summary>The --base-label for a world built on these mods (<see cref="CuratedMods.BaseLabelOf"/>, by content, never by
        /// folder name): none for the game alone, "nocturne@&lt;version&gt;" for a recognised Nocturne in any folder, "m:" + 8 hex
        /// for any other mod, in load order. Not shareable (no lobby label) when a base folder is missing, unreadable or empty,
        /// two have the same files, or the part would not fit the generator's 180 characters.</summary>
        public static (bool Shareable, string? Label) BaseLabel(string modsDir, IReadOnlyList<string> baseMods)
        {
            try
            {
                var label = CuratedMods.BaseLabelOf(modsDir, baseMods);
                return label is { Length: > 180 } ? (false, null) : (true, label);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                return (false, null);
            }
        }

        /// <summary>An earlier launcher's save folder of this world (<see cref="WorldSaves.LegacyFolder"/>).</summary>
        public static string SaveFolder(string profileFolder, World world) =>
            Path.Combine(profileFolder, "randomizer", world.Code + " " + world.Hash);

        /// <summary>The generator's arguments. update: the game's update folder when it is kept beside the game
        /// (GameCheck.LaidOverGame); the engine lays it over the game, so the generator must read through it too, or a
        /// game with its update beside it builds its world from the unpatched files (another world than a host's).</summary>
        public static List<string> GenerateArguments(string game, string code, string modsDir, IEnumerable<string> modFolders, string? baseLabel = null,
            string? update = null)
        {
            var args = new List<string> { "generate", "--game", game, "--code", code, "--out", modsDir };
            if (update != null) args.AddRange(new[] { "--update", update });
            if (baseLabel != null) args.AddRange(new[] { "--base-label", baseLabel });
            var mods = modFolders.ToList();
            if (mods.Count > 0)
            {
                args.Add("--mods");
                args.AddRange(mods);
            }
            return args;
        }

        /// <summary>The mod names a world was built on, from its manifest; null when it can't be read.</summary>
        public static List<string>? BuiltOn(string worldFolder)
        {
            try
            {
                var manifest = JsonNode.Parse(File.ReadAllText(Path.Combine(worldFolder, "randomizer_manifest.json")));
                return manifest?["mods"]?.AsArray().Select(n => n?.GetValue<string>() ?? "").ToList();
            }
            catch
            {
                return null;
            }
        }

        /// <summary>"Built on Nocturne." for a world's card, so a friend knows which mods to use with the code.</summary>
        public static string BuiltOnText(List<string>? builtOn) =>
            builtOn == null ? L.T("Built on: unknown.")
            : builtOn.Count == 0 ? L.T("Built on the game alone.")
            : L.F($"Built on {string.Join(", ", builtOn)}.");

        /// <summary>Option coiled_host has something to act on: one of these mod folders places the Coiled Host (Nocturne
        /// v2.0.0 or later), read from its files like the generator does, so a copy from Nexus or any folder name counts, not
        /// only one the launcher downloaded. Reads a map file, so call it off the UI thread.</summary>
        public static bool HasCoiledHost(string modsDir, IEnumerable<string> modFolders) =>
            modFolders.Any(m => PlacesCoiledHost(Path.Combine(modsDir, m)));

        /// <summary>Nocturne's Coiled Host: entity 2700806 in m27_00_00_00's map (DATA: once in Nocturne 2.0.0 and 3.0.0,
        /// never in the game's own m27 map).</summary>
        public const int CoiledHostEntity = 2700806;

        /// <summary>The mod ships m27_00_00_00's map and it holds the Coiled Host's entity id; false for anything unreadable.</summary>
        public static bool PlacesCoiledHost(string folder) => PlacesEntity(folder, "m27_00_00_00", CoiledHostEntity);

        /// <summary>Option scorched_beast has something to act on: a ticked mod places the Scorched Beast (Nocturne v5.0.0 or
        /// later), found the same way as the Coiled Host. Reads a map file, so call it off the UI thread.</summary>
        public static bool HasScorchedBeast(string modsDir, IEnumerable<string> modFolders) =>
            modFolders.Any(m => PlacesScorchedBeast(Path.Combine(modsDir, m)));

        /// <summary>Nocturne's Scorched Beast (the Lesser Demon): entity 2300820 in m23_00_00_00's map (in no game map).</summary>
        public const int ScorchedBeastEntity = 2300820;

        public static bool PlacesScorchedBeast(string folder) => PlacesEntity(folder, "m23_00_00_00", ScorchedBeastEntity);

        /// <summary>The mod ships this map and its bytes hold the entity id (a 4-byte search); false for anything unreadable.</summary>
        public static bool PlacesEntity(string folder, string map, int entity)
        {
            try
            {
                if (!Directory.Exists(folder)) return false;
                var root = CuratedMods.ContentRoot(folder);
                var dvd = Path.Combine(root, "dvdroot_ps4");
                var file = Path.Combine(Directory.Exists(dvd) ? dvd : root, "map", "MapStudio", map + ".msb.dcx");
                if (!File.Exists(file)) return false;
                var key = BitConverter.GetBytes(entity);
                return Dcx(File.ReadAllBytes(file)).AsSpan().IndexOf(key) >= 0;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { return false; }
        }

        /// <summary>A DCX file's data (DFLT: zlib at 0x4C, big-endian sizes at 0x1C / 0x20); bytes without a DCX header as they are.</summary>
        internal static byte[] Dcx(byte[] file)
        {
            if (file.Length < 4 || file[0] != 'D' || file[1] != 'C' || file[2] != 'X' || file[3] != 0) return file;
            if (file.Length < 0x4C || Encoding.ASCII.GetString(file, 0x28, 4) != "DFLT")
                throw new InvalidDataException("unsupported DCX compression");
            var size = System.Buffers.Binary.BinaryPrimitives.ReadInt32BigEndian(file.AsSpan(0x1C));
            var packed = System.Buffers.Binary.BinaryPrimitives.ReadInt32BigEndian(file.AsSpan(0x20));
            if (size < 0 || packed < 0 || 0x4C + (long)packed > file.Length) throw new InvalidDataException("bad DCX sizes");
            using var z = new System.IO.Compression.ZLibStream(new MemoryStream(file, 0x4C, packed), System.IO.Compression.CompressionMode.Decompress);
            var data = new byte[size];
            z.ReadExactly(data);
            return data;
        }

        /// <summary>The mods a new world is built on, in load order: every installed mod but the worlds, ticked by the page's
        /// choice, else by its My mods state. A world loads its own base mods (its manifest), so a mod that is off in My mods
        /// still counts once ticked (an updated Nocturne's new folder starts off there).</summary>
        public static List<string> Picked(IEnumerable<Mods.Entry> mods, IReadOnlyDictionary<string, bool> ticks) =>
            mods.Where(m => !IsWorld(m.Name) && (ticks.TryGetValue(m.Name, out var ticked) ? ticked : m.Enabled))
                .Select(m => m.Name).ToList();

        /// <summary>Puts this world last and enabled, and turns the other worlds off; other mods keep their state.</summary>
        public static List<Mods.Entry> WithWorld(IEnumerable<Mods.Entry> mods, string worldFolderName) =>
            mods.Where(m => m.Name != worldFolderName)
                .Select(m => IsWorld(m.Name) ? m with { Enabled = false } : m)
                .Append(new Mods.Entry(worldFolderName, true)).ToList();

        /// <summary>The id BuildIdAsync found (tests set it: no generator runs there).</summary>
        internal static string? KnownBuildId;

        /// <summary>This package's generator build id ("randomizer build-id": 64 hex, the "g:" of the labels it writes), asked
        /// once per launcher run; null when the generator can't tell (an older one, or it failed).</summary>
        public static async Task<string?> BuildIdAsync(CancellationToken cancel = default)
        {
            if (KnownBuildId != null) return KnownBuildId;
            try
            {
                var id = (await Run(new[] { "build-id" }, cancel: cancel)).Trim();
                if (id.Length == 64 && id.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f')) KnownBuildId = id;
            }
            catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception or IOException)
            {
            }
            return KnownBuildId;
        }

        /// <summary>The page's starting choices: enemies change, drops shuffle, bosses stay.</summary>
        public static Options Standard(ulong seed) =>
            new(seed, 100, "slot", "slot", Array.Empty<int>(), Drops: true);

        /// <summary>A valid code's options as the page knows them. Hidden: the code also sets something the page has no row for
        /// (per-area chances, one model everywhere, fixed scaling, variety, map caps); a world built from the code keeps it.</summary>
        public static (Options Options, bool Hidden) FromCode(WorldCodeResult code)
        {
            if (code is not { Kind: WorldCodeKind.Ok, Options: { } o }) throw new ArgumentException("not a valid code", nameof(code));
            long I(string key) => (long)o[key];
            bool B(string key) => I(key) != 0;
            var options = new Options(code.Seed, (int)I("enemy_chance"), I("scaling") switch { 1 => "off", 2 => "fixed", _ => "slot" },
                B("rewards") ? "replacement" : "slot", ((long[])o["exclude"]).Select(x => (int)x).ToList(), B("boss_swap"), B("treasure"),
                B("shops"), B("drops"), B("treasure_by_type"), B("starters"), (int)I("headroom"),
                B("chalice_materials"), (int)I("starter_melee"), (int)I("starter_guns"), B("witch_mix"), B("shadows_mix"),
                B("emissary_mix"), B("failures_mix"), BellMaidenChoices[(int)I("bell_maidens")], B("chalice_bosses"), B("hard_rom"),
                B("hard_amygdala"), B("hard_ebrietas"), B("difficult_enemies"), B("second_stage"), B("away_from_home"), B("free_poses"),
                B("starter_armor"), B("shadows_summons"), B("chalice_enemies"), B("bosses_as_enemies"), B("coiled_host"), B("gem_roulette"),
                B("cursed_gems"), (int)I("boss_enemy_chance"), (int)I("boss_enemy_max"), B("huge_bosses_as_enemies"),
                (int)I("boss_enemy_hp"), B("no_winter_lanterns"), ((long[])o["include_only"]).Select(x => (int)x).ToList(), B("scorched_beast"));
            var hidden = ((ValueTuple<long, long>[])o["area_chance"]).Length > 0 || I("oops_all") != 0 || I("fixed_effect") != 0 ||
                         I("variety") != 0 || I("map_models") != 24 || I("map_mb") != 343;
            return (options, hidden);
        }

        /// <summary>The code these options make, worked out here so the page shows it as rows change (the generator decodes
        /// it and refuses any spelling it would not write itself). Kept: the settings of <paramref name="from"/> that have no
        /// row (a pasted code's per-area chances, map caps and the like). Null when the seed is above 32 bits.</summary>
        public static string? ToCode(Options o, IReadOnlyDictionary<string, object>? from = null)
        {
            if (o.Seed > uint.MaxValue) return null;
            var c = from != null ? new Dictionary<string, object>(from) : WorldCode.Defaults();
            static long B(bool b) => b ? 1 : 0;
            c["enemy_chance"] = (long)o.Chance;
            c["scaling"] = o.Scaling switch { "off" => 1L, "fixed" => 2L, _ => 0L };
            if (o.Scaling != "fixed") c["fixed_effect"] = 0L;
            c["rewards"] = B(o.Rewards == "replacement");
            c["exclude"] = o.Exclude.Select(x => (long)x).ToArray();
            c["boss_swap"] = B(o.Bosses);
            c["treasure"] = B(o.Treasure);
            c["shops"] = B(o.Shops);
            c["drops"] = B(o.Drops);
            c["treasure_by_type"] = B(o.TreasureByType);
            c["starters"] = B(o.Starters);
            c["headroom"] = (long)o.Headroom;
            c["chalice_materials"] = B(o.ChaliceMaterials);
            c["starter_melee"] = (long)o.StarterMelee;
            c["starter_guns"] = (long)o.StarterGuns;
            c["witch_mix"] = B(o.WitchMix);
            c["shadows_mix"] = B(o.ShadowsMix);
            c["emissary_mix"] = B(o.EmissaryMix);
            c["failures_mix"] = B(o.FailuresMix);
            c["bell_maidens"] = (long)Array.IndexOf(BellMaidenChoices, o.BellMaidens);
            c["chalice_bosses"] = B(o.ChaliceBosses);
            c["hard_rom"] = B(o.HardRom);
            c["hard_amygdala"] = B(o.HardAmygdala);
            c["hard_ebrietas"] = B(o.HardEbrietas);
            c["difficult_enemies"] = B(o.DifficultEnemies);
            c["second_stage"] = B(o.SecondStage);
            c["away_from_home"] = B(o.AwayFromHome);
            c["free_poses"] = B(o.FreePoses);
            c["starter_armor"] = B(o.StarterArmor);
            c["shadows_summons"] = B(o.ShadowsSummons);
            c["chalice_enemies"] = B(o.ChaliceEnemies);
            c["gem_roulette"] = B(o.GemRoulette);
            c["cursed_gems"] = B(o.CursedGems);
            c["bosses_as_enemies"] = B(o.BossesAsEnemies);
            c["boss_enemy_chance"] = (long)o.BossEnemyChance;
            c["boss_enemy_max"] = (long)o.BossEnemyMax;
            c["huge_bosses_as_enemies"] = B(o.HugeBossesAsEnemies);
            c["boss_enemy_hp"] = (long)o.BossEnemyHp;
            c["coiled_host"] = B(o.CoiledHost);
            c["no_winter_lanterns"] = B(o.NoWinterLanterns);
            c["scorched_beast"] = B(o.ScorchedBeast);
            c["include_only"] = (o.IncludeOnly ?? Array.Empty<int>()).Select(x => (long)x).ToArray();
            return WorldCode.Encode((uint)o.Seed, c);
        }

        /// <summary>One line for a world's card and the code preview: what changes, in short words.</summary>
        public static string Summary(Options o, bool hidden = false)
        {
            var parts = new List<string> { o.Chance == 100 ? L.T("enemies") : L.F($"{o.Chance} % of enemies") };
            if (o.Bosses) parts.Add(L.T("boss arenas"));
            if (o.Treasure) parts.Add(L.T("treasure"));
            if (o.Shops) parts.Add(L.T("shops"));
            if (o.Drops) parts.Add(L.T("drops"));
            if (o.GemRoulette) parts.Add(o.CursedGems ? L.T("blood gems (cursed ones too)") : L.T("blood gems"));
            if (o.StarterArmor) parts.Add(L.T("starting armor"));
            if (o.ShadowsSummons) parts.Add(L.T("random summons"));
            if (o.BossesAsEnemies) parts.Add(L.T("bosses as enemies"));
            if (o.Starters) parts.Add(L.T("starting weapons"));
            if (o.ChaliceMaterials) parts.Add(L.T("chalice materials"));
            var text = L.T("Shuffled: ") + string.Join(", ", parts) + ".";
            if (o.Headroom != 100) text += " " + L.F($"Variety {o.Headroom} %.");
            var mixed = new List<string>();
            if (o.WitchMix) mixed.Add(L.T("Witch of Hemwick"));
            if (o.ShadowsMix) mixed.Add(L.T("Shadows of Yharnam"));
            if (o.EmissaryMix) mixed.Add(L.T("Celestial Emissary"));
            if (o.FailuresMix) mixed.Add(L.T("Living Failures"));
            if (o.Bosses && mixed.Count > 0) text += " " + L.T("Mixed fights: ") + string.Join(", ", mixed) + ".";
            if (o.Bosses && o.SecondStage) text += " " + L.T("Gascoigne, Ludwig and the Orphan turn into another boss.");
            if (o.Bosses && o.CoiledHost) text += " " + L.T("Coiled Host in the boss swaps.");
            if (o.Bosses && o.ScorchedBeast) text += " " + L.T("Scorched Beast in the boss swaps.");
            if (o.DifficultEnemies) text += " " + L.T("Difficult enemies.");
            if (o.AwayFromHome) text += " " + L.T("Enemies away from home.");
            if (o.FreePoses) text += " " + L.T("More randomness.");
            if (o.ChaliceEnemies) text += " " + L.T("Chalice dungeon enemies.");
            if (o.NoWinterLanterns) text += " " + L.T("No Winter Lanterns.");
            if (o.BellMaidens != "off")
                text += " " + (o.BellMaidens == "difficult" ? L.T("Bell maidens: difficult enemies.") : L.T("Bell maidens: enemies."));
            if ((o.Bosses || o.BossesAsEnemies) && o.ChaliceBosses) text += " " + L.T("Chalice bosses in the pool.");
            if (o.BossesAsEnemies && (o.BossEnemyChance != 25 || o.BossEnemyMax != 1))
                text += " " + L.F($"Bosses as enemies: {o.BossEnemyChance} % of areas, up to {o.BossEnemyMax} each.");
            if (o.BossesAsEnemies && o.HugeBossesAsEnemies) text += " " + L.T("Huge bosses as enemies.");
            if (o.BossesAsEnemies && o.BossEnemyHp != 8) text += " " + L.F($"Boss HP as an enemy: {o.BossEnemyHp}x.");
            if (o.HardRom || o.HardAmygdala || o.HardEbrietas)
            {
                var hard = new List<string>();
                if (o.HardRom) hard.Add(L.T("Rom"));
                if (o.HardAmygdala) hard.Add(L.T("Amygdala"));
                if (o.HardEbrietas) hard.Add(L.T("Ebrietas"));
                text += " " + L.T("Chalice-strength: ") + string.Join(", ", hard) + ".";
            }
            if (o.StarterMelee != 3) text += " " + L.F($"Melee starting weapons offered: {o.StarterMelee}.");
            if (o.StarterGuns != 2) text += " " + L.F($"Firearm starting weapons offered: {o.StarterGuns}.");
            if (o.Exclude.Count > 0) text += " " + L.F($"{o.Exclude.Count} enemies left out.");
            if (o.IncludeOnly is { Count: > 0 } only) text += " " + L.F($"Only {only.Count} kinds of enemies.");
            if (hidden) text += " " + L.T("Also has settings this page has no row for.");
            return text;
        }

        /// <summary>The generator's steps (generate.PROGRESS_STEPS) and their share of the bar; placing enemies takes most.</summary>
        private static readonly (string Step, double From, double To)[] Steps =
        {
            ("read", 0.00, 0.10), ("plan", 0.10, 0.25), ("maps", 0.25, 0.70), ("effects", 0.70, 0.80), ("items", 0.80, 0.88),
            ("text", 0.88, 0.92), ("write", 0.92, 1.00),
        };

        /// <summary>A "progress &lt;step&gt; &lt;done&gt; &lt;total&gt;" line as the fraction of the whole run and its step; null for any other line.</summary>
        public static (double Fraction, string Step)? ParseProgress(string line)
        {
            var parts = line.Trim().Split(' ');
            if (parts.Length != 4 || parts[0] != "progress" || !int.TryParse(parts[2], out var done) || !int.TryParse(parts[3], out var total) || total <= 0)
                return null;
            var step = Steps.FirstOrDefault(x => x.Step == parts[1]);
            if (step.Step == null) return null;
            return (step.From + (step.To - step.From) * Math.Clamp((double)done / total, 0, 1), step.Step);
        }

        public static string StepText(string step) => step switch
        {
            "read" => L.T("Reading your game files…"),
            "plan" => L.T("Choosing enemies…"),
            "maps" => L.T("Placing enemies in the maps…"),
            "items" => L.T("Shuffling items…"),
            "text" => L.T("Writing item text…"),
            "effects" => L.T("Copying enemy effects…"),
            "write" => L.T("Writing the world…"),
            _ => L.T("Generating…"),
        };

        /// <summary>The generator's exit codes (randomizer.py): 2 outside Nightwatch, 3 a code from a newer generator, 4 not a code,
        /// 5 the world's folder is in use (nothing was changed).</summary>
        public static string? ExitText(int code) => code switch
        {
            2 => L.T("The generator only runs inside Nightwatch Co-op."),
            3 => L.T("This code comes from a newer randomizer. Update the launcher to use it."),
            4 => L.T("That is not a valid world code. Check it for typing mistakes."),
            5 => L.T("That world's folder is in use. Close the game and try again."),
            _ => null,
        };

        /// <summary>Generates a world on these base mods (in load order) and returns its folder name; the game's files are only
        /// read. The caller picks it with <see cref="Use"/>. progress gets the fraction done and the step's text as the generator
        /// reports them. Cancelling stops the generator and removes its unfinished folder (an OperationCanceledException).</summary>
        public static async Task<string> Generate(LauncherSettings s, string code, IReadOnlyList<string> modNames,
            Action<double, string>? progress = null, CancellationToken cancel = default)
        {
            var root = GameEnvironment.ModsDir(s);
            Directory.CreateDirectory(root);
            var (_, baseLabel) = BaseLabel(root, modNames);
            string output;
            try
            {
                output = await Run(GenerateArguments(GameEnvironment.GameDir(s), code, root, modNames.Select(n => Path.Combine(root, n)), baseLabel,
                    GameCheck.LaidOverGame(GameEnvironment.GameDir(s))),
                    line => { if (ParseProgress(line) is { } p) progress?.Invoke(p.Fraction, StepText(p.Step)); }, cancel);
            }
            catch (OperationCanceledException)
            {
                RemoveUnfinished(root);
                throw;
            }
            var wrote = output.Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.StartsWith("wrote "));
            var name = wrote == null ? null : Path.GetFileName(wrote[6..].TrimEnd('\\', '/'));
            if (name == null || !IsWorld(name)) throw new InvalidOperationException(L.T("The generator ended without writing a world."));
            return name;
        }

        /// <summary>The generator writes "&lt;world&gt;.tmp" and renames it when done; a stopped run leaves that folder behind.</summary>
        public static void RemoveUnfinished(string modsDir)
        {
            if (!Directory.Exists(modsDir)) return;
            foreach (var folder in Directory.EnumerateDirectories(modsDir, "Randomizer *.tmp"))
                if (IsWorld(Path.GetFileName(folder)[..^4]))
                    try { Directory.Delete(folder, recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }

        private static async Task<string> Run(IEnumerable<string> arguments, Action<string>? line = null, CancellationToken cancel = default)
        {
            var info = new ProcessStartInfo(Generator)
            {
                WorkingDirectory = Path.GetDirectoryName(Generator)!,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            foreach (var argument in arguments) info.ArgumentList.Add(argument);
            cancel.ThrowIfCancellationRequested();
            using var process = Process.Start(info)!;
            using var stop = cancel.Register(() =>
            {
                try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { }
            });
            var errors = process.StandardError.ReadToEndAsync();
            var output = new StringBuilder();
            while (await process.StandardOutput.ReadLineAsync() is { } text)
            {
                output.AppendLine(text);
                line?.Invoke(text);
            }
            await process.WaitForExitAsync();
            cancel.ThrowIfCancellationRequested();
            if (process.ExitCode != 0)
            {
                var last = (await errors).Split('\n').Select(l => l.Trim()).LastOrDefault(l => l.Length > 0);
                throw new InvalidOperationException(ExitText(process.ExitCode) ?? last ?? L.T("The generator failed."));
            }
            return output.ToString();
        }
    }
}
