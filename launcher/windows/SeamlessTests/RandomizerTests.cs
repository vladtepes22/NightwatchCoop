using System.IO;
using System.Text.Json.Nodes;
using System.Text;
using BloodborneLauncher.Core;

internal static class RandomizerTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException("Randomizer: " + message);
    }

    internal static void Run()
    {
        var world = Randomizer.Parse("Randomizer R2-0M000-01G74-ZY 227291f8");
        Check(world is { Code: "R2-0M000-01G74-ZY", Hash: "227291f8" }, "a world's folder name reads back");
        foreach (var bad in new[] { "Randomizer", "Randomizer R2 227291f8", "Randomizer r2-00 227291f8", "Randomizer R2-00 22729", "Randomizer R2-00/.. 227291f8", "Cinder Rain", "Randomizer R2-00 227291f8 " })
            Check(Randomizer.Parse(bad) == null, "not a world: " + bad);

        var folder = Randomizer.SaveFolder(Path.Combine("saves", "profiles", "Anna"), world!);
        Check(folder == Path.Combine("saves", "profiles", "Anna", "randomizer", "R2-0M000-01G74-ZY 227291f8"), "save folder under the profile");
        Check(Path.GetFileName(folder).All(c => char.IsLetterOrDigit(c) || c is '-' or ' '), "save folder has only safe characters");

        Check(Randomizer.BuiltOnText(new List<string> { "Nocturne", "Embers" }) == "Built on Nocturne, Embers.", "built on two mods");
        Check(Randomizer.BuiltOnText(new List<string>()) == "Built on the game alone.", "built on no mods");
        Check(Randomizer.BuiltOnText(null) == "Built on: unknown.", "manifest unreadable");

        Check(!Randomizer.GenerateArguments("g", "c", "m", Array.Empty<string>()).Contains("--mods"), "no mods, no --mods");
        Check(string.Join(" ", Randomizer.GenerateArguments("g", "c", "m", new[] { "a", "b" })) == "generate --game g --code c --out m --mods a b", "mods keep load order");


        const string a = "Randomizer R2-00 aaaaaaaa", b = "Randomizer R2-11 bbbbbbbb";
        var mods = new List<Mods.Entry> { new(a, true), new("Cinder", true), new(b, false), new("Off", false) };
        var after = Randomizer.WithWorld(mods, b);
        Check(after[^1] == new Mods.Entry(b, true), "the new world loads last and on");
        Check(after.Single(m => m.Name == a).Enabled == false, "other worlds go off");
        Check(after.Single(m => m.Name == "Cinder").Enabled && !after.Single(m => m.Name == "Off").Enabled, "other mods keep their state");

        Check(!Randomizer.IsStale(new[] { "Cinder" }, mods, a), "same list: not stale");
        Check(Randomizer.IsStale(Array.Empty<string>(), mods, a), "a mod added: stale");
        Check(Randomizer.IsStale(new[] { "Cinder", "Off" }, mods, a), "a mod removed: stale");
        Page();
        EndToEnd();
        Console.WriteLine("Randomizer tests passed");
    }

    /// <summary>The code the page shows as rows change: the launcher's encoder writes every vector's canonical spelling, the page's
    /// options make it back (with the pasted code's settings that have no row), and an edit after a paste keeps those.</summary>
    private static void Codes(JsonNode vectors)
    {
        foreach (var v in vectors["vectors"]!.AsArray())
        {
            if (v!["canonical"]?.GetValue<string>() is not { } canonical) continue;
            var decoded = WorldCode.Decode(v["code"]!.GetValue<string>());
            var name = v["name"]!.GetValue<string>();
            Check(WorldCode.Encode(decoded.Seed, decoded.Options!) == canonical, "the encoder writes the canonical code: " + name);
            var (options, _) = Randomizer.FromCode(decoded);
            Check(Randomizer.ToCode(options, decoded.Options) == canonical, "the page's options make the pasted code again: " + name);
            if (decoded.Options!.Values.Any(o => o is (ValueTuple<long, long>[] { Length: > 0 })))
            {
                var edited = WorldCode.Decode(Randomizer.ToCode(options with { Treasure = !options.Treasure }, decoded.Options));
                Check(edited.Kind == WorldCodeKind.Ok && ((ValueTuple<long, long>[])edited.Options!["area_chance"]).SequenceEqual((ValueTuple<long, long>[])decoded.Options["area_chance"]) &&
                      (long)edited.Options["treasure"] != (long)decoded.Options["treasure"], "a row changed after a paste keeps the pasted code's per-area chances: " + name);
            }
        }
        var standard = WorldCode.Decode(Randomizer.ToCode(Randomizer.Standard(7)));
        Check(standard is { Kind: WorldCodeKind.Ok, Seed: 7 } && (long)standard.Options!["drops"] == 1 && (long)standard.Options["boss_swap"] == 0,
            "the standard options make a code of their own");
        Check(Randomizer.ToCode(Randomizer.Standard(5)) != Randomizer.ToCode(Randomizer.Standard(6)), "another seed is another code");
        Check(Randomizer.ToCode(Randomizer.Standard(5)) != Randomizer.ToCode(Randomizer.Standard(5) with { Bosses = true }), "another option is another code");
        Check(Randomizer.ToCode(Randomizer.Standard(1UL << 32)) == null, "a seed above 32 bits makes no code");
        Check(Randomizer.ToCode(Randomizer.Standard(5) with { Chance = 33 }) == null, "an option out of range makes no code");
        var fixedCode = vectors["vectors"]!.AsArray().First(v => v!["name"]!.GetValue<string>().Contains("scaling fixed"))!["canonical"]!.GetValue<string>();
        var fixedDecoded = WorldCode.Decode(fixedCode);
        var unfixed = WorldCode.Decode(Randomizer.ToCode(Randomizer.FromCode(fixedDecoded).Options with { Scaling = "slot" }, fixedDecoded.Options));
        Check(unfixed.Kind == WorldCodeKind.Ok && (long)unfixed.Options!["fixed_effect"] == 0, "leaving fixed scaling drops its effect");
    }

    /// <summary>The page's model: codes fill the options, presets, the card line, the generator's progress lines, the
    /// spoiler's readable part and the clean-up after a stopped run.</summary>
    /// <summary>Option coiled_host's row (needs no generator vectors).</summary>
    private static void CoiledHost()
    {
        // option coiled_host: the page greys it out unless a ticked mod places the Coiled Host (read from its m27 map, so a
        // Nexus copy under any folder name counts, without the launcher's nocturne.json)
        var noMods = Path.Combine(Path.GetTempPath(), "seamless-coiled-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(noMods, "Nocturne v2.0.0"));
        static byte[] DcxOf(byte[] data)
        {
            var packed = new MemoryStream();
            using (var z = new System.IO.Compression.ZLibStream(packed, System.IO.Compression.CompressionLevel.Optimal, true)) z.Write(data);
            var file = new byte[0x4C + packed.Length];
            Encoding.ASCII.GetBytes("DCX\0").CopyTo(file, 0);
            System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(file.AsSpan(0x1C), data.Length);
            System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(file.AsSpan(0x20), (int)packed.Length);
            Encoding.ASCII.GetBytes("DFLT").CopyTo(file, 0x28);
            packed.ToArray().CopyTo(file, 0x4C);
            return file;
        }
        void Map(string mod, string? wrapper, int entity)
        {
            var dir = Path.Combine(new[] { noMods, mod, wrapper ?? "", "dvdroot_ps4", "map", "MapStudio" }.Where(p => p.Length > 0).ToArray());
            Directory.CreateDirectory(dir);
            var data = new byte[64];
            BitConverter.GetBytes(entity).CopyTo(data, 20);
            File.WriteAllBytes(Path.Combine(dir, "m27_00_00_00.msb.dcx"), DcxOf(data));
        }
        try
        {
            Check(!Randomizer.HasCoiledHost(noMods, Array.Empty<string>()), "no mods ticked: no Coiled Host");
            Check(!Randomizer.HasCoiledHost(noMods, new[] { "Nocturne v2.0.0", "missing" }), "a folder named Nocturne without its files is not Nocturne");
            Map("from nexus", null, Randomizer.CoiledHostEntity);
            Check(Randomizer.HasCoiledHost(noMods, new[] { "missing", "from nexus" }), "a mod whose m27 map places the Coiled Host has him, whatever its folder is called");
            Map("wrapped", "CUSA03173", Randomizer.CoiledHostEntity);
            Check(Randomizer.HasCoiledHost(noMods, new[] { "wrapped" }), "the Coiled Host found under a CUSA03173 wrapper");
            Map("other m27", null, Randomizer.CoiledHostEntity + 1);
            Check(!Randomizer.HasCoiledHost(noMods, new[] { "other m27" }), "an m27 map without his entity id has no Coiled Host");
            File.WriteAllBytes(Path.Combine(noMods, "other m27", "dvdroot_ps4", "map", "MapStudio", "m27_00_00_00.msb.dcx"), Encoding.ASCII.GetBytes("DCX\0broken"));
            Check(!Randomizer.HasCoiledHost(noMods, new[] { "other m27" }), "a broken map file is no Coiled Host (no crash)");
            // a real Nocturne build when given (SAVEEDIT_NOCTURNE, as the save editor tests): it places him
            if (Environment.GetEnvironmentVariable("SAVEEDIT_NOCTURNE") is { Length: > 0 } real && Directory.Exists(real))
                Check(Randomizer.PlacesCoiledHost(real), "the real Nocturne build places the Coiled Host: " + Path.GetFileName(real));
        }
        finally { Directory.Delete(noMods, true); }
    }

    private static void Page()
    {
        CoiledHost(); // needs no generator vectors, so it runs on the public branches too
        if (WorldJoinTests.GeneratorVectors() is not { } vectors) return;
        var checkedOne = false;
        foreach (var v in vectors["vectors"]!.AsArray())
        {
            if (v!["canonical"] == null) continue;
            var o = v["options"]!.AsObject();
            long I(string key) => o[key]!.GetValue<long>();
            var (options, hidden) = Randomizer.FromCode(WorldCode.Decode(v["code"]!.GetValue<string>()));
            var name = v["name"]!.GetValue<string>();
            Check(options.Seed == v["seed"]!.GetValue<ulong>() && options.Chance == I("enemy_chance") && options.Headroom == I("headroom") &&
                  options.WitchMix == (I("witch_mix") == 1) && options.ShadowsMix == (I("shadows_mix") == 1) &&
                  options.EmissaryMix == (I("emissary_mix") == 1) && options.FailuresMix == (I("failures_mix") == 1) &&
                  options.BellMaidens == Randomizer.BellMaidenChoices[I("bell_maidens")] && options.ChaliceBosses == (I("chalice_bosses") == 1) &&
                  options.HardRom == (I("hard_rom") == 1) && options.HardAmygdala == (I("hard_amygdala") == 1) && options.HardEbrietas == (I("hard_ebrietas") == 1) &&
                  options.DifficultEnemies == (I("difficult_enemies") == 1) && options.SecondStage == (I("second_stage") == 1) && options.AwayFromHome == (I("away_from_home") == 1) && options.FreePoses == (I("free_poses") == 1) && options.StarterArmor == (I("starter_armor") == 1) && options.ShadowsSummons == (I("shadows_summons") == 1) && options.ChaliceEnemies == (I("chalice_enemies") == 1) && options.GemRoulette == (I("gem_roulette") == 1) && options.CursedGems == (I("cursed_gems") == 1) && options.BossesAsEnemies == (I("bosses_as_enemies") == 1) && options.CoiledHost == (I("coiled_host") == 1) && options.BossEnemyChance == I("boss_enemy_chance") && options.BossEnemyMax == I("boss_enemy_max") && options.BossEnemyHp == I("boss_enemy_hp") && options.HugeBossesAsEnemies == (I("huge_bosses_as_enemies") == 1) && options.NoWinterLanterns == (I("no_winter_lanterns") == 1) && options.StarterMelee == I("starter_melee") && options.StarterGuns == I("starter_guns") && options.Bosses == (I("boss_swap") == 1) && options.Treasure == (I("treasure") == 1) &&
                  options.Shops == (I("shops") == 1) && options.Drops == (I("drops") == 1) && options.Starters == (I("starters") == 1) &&
                  options.TreasureByType == (I("treasure_by_type") == 1) &&
                  options.ChaliceMaterials == (I("chalice_materials") == 1) &&
                  options.Scaling == (I("scaling") switch { 1 => "off", 2 => "fixed", _ => "slot" }) &&
                  options.Rewards == (I("rewards") == 1 ? "replacement" : "slot") &&
                  options.Exclude.SequenceEqual(o["exclude"]!.AsArray().Select(x => x!.GetValue<int>())), "a code fills the page's options: " + name);
            var expectHidden = o["area_chance"]!.AsArray().Count > 0 || I("oops_all") != 0 || I("fixed_effect") != 0 || I("variety") != 0 ||
                               I("map_models") != 24 || I("map_mb") != 343;
            Check(hidden == expectHidden, "a code's settings without a row are noticed: " + name);
            checkedOne = true;
        }
        Check(checkedOne, "the vectors hold valid codes");
        Codes(vectors);
        Check(EnemyCatalog.All.Length > 0 && EnemyCatalog.All.Select(e => e.Model).Distinct().Count() == EnemyCatalog.All.Length &&
              EnemyCatalog.All.All(e => e.Name.Length > 0 && e.Areas.Length > 0), "the enemy picker's catalogue: one row per model, each named and placed");
        var refused = false;
        try { Randomizer.FromCode(WorldCode.Decode("R2-not-a-code")); } catch (ArgumentException) { refused = true; }
        Check(refused, "a bad code fills nothing");

        Check(Randomizer.Summary(Randomizer.Standard(1)) == "Shuffled: enemies, drops.", "the card line of the standard world");
        Check(Randomizer.Summary(Randomizer.Standard(1) with { StarterMelee = 4, StarterGuns = 3 }) ==
              "Shuffled: enemies, drops. Melee starting weapons offered: 4. Firearm starting weapons offered: 3.", "the card line names the starting weapon counts");
        Check(Randomizer.Summary(Randomizer.Standard(1) with { Chance = 50, Treasure = true, Headroom = 150, Exclude = new[] { 1, 2 } }, hidden: true) ==
              "Shuffled: 50 % of enemies, treasure, drops. Variety 150 %. 2 enemies left out. Also has settings this page has no row for.", "the card line names the options");

        Check(Randomizer.ParseProgress("progress read 0 1") is { Fraction: 0.0, Step: "read" }, "the first step starts the bar");
        Check(Randomizer.ParseProgress("progress maps 8 16") is { Step: "maps" } half && Math.Abs(half.Fraction - 0.475) < 1e-9, "maps fill their share");
        Check(Randomizer.ParseProgress("progress write 1 1") is { Fraction: 1.0 }, "the last step ends the bar");
        Check(Randomizer.ParseProgress(@"wrote C:\x") == null && Randomizer.ParseProgress("progress nope 1 2") == null &&
              Randomizer.ParseProgress("progress maps 1 0") == null, "other lines are no progress");
        Check(Randomizer.ExitText(3) != null && Randomizer.ExitText(4) != null && Randomizer.ExitText(1) == null, "the generator's exit codes have words");

        var spoiler = WorldSpoiler.Parse(JsonNode.Parse("""
            {"readable": {"areas": {"m24_01": "Central Yharnam"}, "models": {"c2090": "Blood-starved Beast"},
              "enemies": {"m24_01": [["c1000", "c2090", 3]]}, "bosses": [["m24_01", "c2090", "c1000"]],
              "treasure": [["m24_01", "Pebble x4", "Blood Vial"]], "shops": [[1, "Blood Vial", "Sedative"]],
              "drops": [[["c2090", "c1100"], "nothing 68%, Blood Vial x2 32%", "Pebble"]], "starters": [["Saw Cleaver", "Gatling Gun"]]}}
            """))!;
        Check(spoiler.Enemies.Single() == new WorldSpoiler.Enemy("Central Yharnam", "c1000", "c2090", 3), "enemy changes name their area");
        Check(spoiler.Bosses.Single().Map == "Central Yharnam" && spoiler.Treasure.Single() == new WorldSpoiler.Trade("Central Yharnam", "Pebble x4", "Blood Vial"), "bosses and treasure read");
        Check(spoiler.Shops.Single().Where == "Bath Messengers" && spoiler.Starters.Single().Now == "Gatling Gun", "shops and starting weapons read");
        Check(spoiler.Drops.Single().Where == "Blood-starved Beast (c2090), c1100", "drops name the enemies that drop them");
        Check(WorldSpoiler.Parse(JsonNode.Parse("""{"code": "R2-x"}""")) == null, "an older world has no readable spoiler");
        Check(WorldSpoiler.Parse(JsonNode.Parse("""{"readable": {"error": "KeyError: x"}}""")) is { Error: "KeyError: x", Enemies.Count: 0 }, "a spoiler without names says why");

        var mods = Path.Combine(Path.GetTempPath(), "randomizer-page-" + Guid.NewGuid().ToString("N"));
        try
        {
            foreach (var folder in new[] { "Randomizer R2-0W000-000G2-00MXE 12345678.tmp", "Randomizer R2-0W000-000G2-00MXE 12345678", "Other.tmp" })
                Directory.CreateDirectory(Path.Combine(mods, folder));
            Randomizer.RemoveUnfinished(mods);
            Check(Directory.GetDirectories(mods).Select(Path.GetFileName).OrderBy(n => n).SequenceEqual(new[] { "Other.tmp", "Randomizer R2-0W000-000G2-00MXE 12345678" }),
                "a stopped run's folder goes, finished worlds and other folders stay");
            Randomizer.RemoveUnfinished(Path.Combine(mods, "missing"));
        }
        finally
        {
            Directory.Delete(mods, true);
        }
    }

    /// <summary>Only with BB_RANDOMIZER_E2E = "package folder|game folder": the launcher's calls drive the real generator
    /// into a temporary folder. Nothing else in the package is touched.</summary>
    private static void EndToEnd()
    {
        if (Environment.GetEnvironmentVariable("BB_RANDOMIZER_E2E")?.Split('|') is not [var package, var game]) return;
        var root = Path.Combine(Path.GetTempPath(), "randomizer-e2e-" + Guid.NewGuid().ToString("N"));
        var (oldPort, oldData) = (Paths.PortDir, Environment.GetEnvironmentVariable("BB_DATA_DIR"));
        try
        {
            Directory.CreateDirectory(root);
            Paths.PortDir = package;
            Environment.SetEnvironmentVariable("BB_DATA_DIR", root);
            Check(Randomizer.Supported, "the package has a generator");
            var settings = new LauncherSettings();
            settings.Set("game_dir", game);
            settings.Set("mods_dir", Path.Combine(root, "mods"));
            settings.Set("mods_enabled", true);
            var options = new Randomizer.Options(4242, 100, "slot", "slot", new[] { 2630 }, Bosses: true, Treasure: true, Shops: true, Drops: true);
            var code = Randomizer.ToCode(options)!;
            var name = Randomizer.Generate(settings, code, Array.Empty<string>()).GetAwaiter().GetResult();
            var world = Randomizer.Parse(name);
            Check(world != null && world.Code == code, "the new folder reads as this world: " + name);
            Check(Mods.Load(GameEnvironment.ModsDir(settings)).Last() == new Mods.Entry(name, true), "the world loads last and on");
            Check(Randomizer.Active(settings) == world, "the world is the active one");
            var built = Randomizer.BuiltOn(Path.Combine(GameEnvironment.ModsDir(settings), name));
            Check(built is { Count: 0 }, "the manifest reads back: built on no mods");
            var failed = false;
            try { Randomizer.Generate(settings, "R2-not-a-code", Array.Empty<string>()).GetAwaiter().GetResult(); }
            catch (InvalidOperationException) { failed = true; }
            Check(failed, "a bad code fails with a message");
            Console.WriteLine("Randomizer end to end passed: " + name);
        }
        finally
        {
            Paths.PortDir = oldPort;
            Environment.SetEnvironmentVariable("BB_DATA_DIR", oldData);
            Directory.Delete(root, true);
        }
    }
}
