namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The enemies the randomizer can place (the generator's census of the game alone, 5 Nov 2026: models with AI of their
    /// own that some map places as an ordinary enemy), for the "Never use these enemies" picker. The game has no names
    /// for ordinary enemies, so the names are ours; Guess marks a name not yet checked in game. A mod that adds
    /// enemies is not listed here; the picker's search box still takes any model number.
    /// </summary>
    public static class EnemyCatalog
    {
        public sealed record Enemy(int Model, string Name, bool Guess, bool Difficult, string[] Areas)
        {
            public string Code => "c" + Model.ToString("0000");
        }

        public static readonly Enemy[] All =
        {
            new(1000, "Wolf Beast", false, true, new[] { "Old Yharnam", "Central Yharnam", "Upper Cathedral Ward" }),
            new(1001, "Skeleton-meat Wolf Beast", false, true, new[] { "Yahar'gul, Unseen Village" }),
            new(1050, "Bell-ringing Woman", false, false, new[] { "Nightmare of Mensis", "Yahar'gul, Unseen Village", "Nightmare Frontier" }),
            new(1060, "Brainsucker", false, false, new[] { "Cathedral Ward", "Central Yharnam", "Upper Cathedral Ward", "Byrgenwerth" }),
            new(1090, "Cloaked Beast Patient", false, false, new[] { "Old Yharnam" }),
            new(1100, "Rat", false, false, new[] { "Central Yharnam", "Research Hall" }),
            new(1110, "Nightmare Apostle (Patches Face)", false, false, new[] { "Nightmare of Mensis", "Byrgenwerth", "Nightmare Frontier" }),
            new(1111, "Nightmare Apostle (Patches Body)", false, false, new[] { "Nightmare of Mensis" }),
            new(1120, "Rotted Corpse", false, false, new[] { "Central Yharnam", "Forbidden Woods" }),
            new(1140, "Hunting Dog", false, false, new[] { "Hemwick Charnel Lane" }),
            new(1170, "Carrion Crow", false, false, new[] { "Hemwick Charnel Lane", "Old Yharnam", "Cathedral Ward", "Central Yharnam", "Upper Cathedral Ward", "Forbidden Woods", "Hunter's Nightmare", "Research Hall" }),
            new(1171, "Carrion Crow (Dog Face)", false, false, new[] { "Nightmare of Mensis" }),
            new(1180, "Wandering Nightmare", false, false, new[] { "Hemwick Charnel Lane" }),
            new(1190, "Wandering Nightmare", false, false, new[] { "Nightmare Frontier" }),
            new(1220, "Cainhurst Gargoyle", false, false, new[] { "Forsaken Cainhurst Castle" }),
            new(1240, "Rabid Dog", false, false, new[] { "Cathedral Ward", "Central Yharnam", "Forbidden Woods", "Yahar'gul, Unseen Village" }),
            new(1241, "Hunting Dog (Shaggy, Crow Face)", false, false, new[] { "Nightmare of Mensis" }),
            new(1250, "Snake Ball", false, false, new[] { "Forbidden Woods" }),
            new(1260, "Small Snake Ball", false, false, new[] { "Forbidden Woods" }),
            new(1270, "Maneater Boar", false, true, new[] { "Central Yharnam", "Forbidden Woods", "Yahar'gul, Unseen Village" }),
            new(1271, "Maneater Boar (Mensis)", false, true, new[] { "Nightmare of Mensis" }),
            new(1290, "Cloaked Beast Patient", false, false, new[] { "Old Yharnam", "Hunter's Nightmare" }),
            new(1400, "Child of Rom", false, false, new[] { "Byrgenwerth" }),
            new(2000, "Executioner", false, false, new[] { "Hemwick Charnel Lane", "Cathedral Ward", "Central Yharnam", "Forbidden Woods", "Yahar'gul, Unseen Village" }),
            new(2020, "Snatcher", false, true, new[] { "Cathedral Ward", "Forbidden Woods", "Yahar'gul, Unseen Village" }),
            new(2090, "Blood-starved Beast", false, true, new[] { "Old Yharnam", "Hunter's Nightmare" }),
            new(2100, "Witch of Hemwick", false, false, new[] { "Hemwick Charnel Lane", "Yahar'gul, Unseen Village", "Hunter's Nightmare" }),
            new(2110, "Garden of Eyes", false, true, new[] { "Byrgenwerth" }),
            new(2120, "Mergo's Attendant", false, false, new[] { "Nightmare of Mensis", "Forbidden Woods" }),
            new(2130, "Bloodlicker", false, false, new[] { "Forsaken Cainhurst Castle" }),
            new(2150, "Silver Lady", false, false, new[] { "Forsaken Cainhurst Castle" }),
            new(2170, "Vermin Host", false, true, new[] { "Forbidden Woods" }),
            new(2180, "Silverbeast", false, false, new[] { "Nightmare of Mensis", "Nightmare Frontier" }),
            new(2190, "Hateful Maggot", false, false, new[] { "Forsaken Cainhurst Castle", "Nightmare of Mensis", "Forbidden Woods" }),
            new(2250, "Mergo's Attendant", false, false, new[] { "Nightmare of Mensis" }),
            new(2260, "Mergo's Chief Attendant", false, false, new[] { "Nightmare of Mensis" }),
            new(2330, "Cainhurst Servant", false, false, new[] { "Forsaken Cainhurst Castle" }),
            new(2400, "Giant Lost Child", false, false, new[] { "Nightmare of Mensis", "Nightmare Frontier" }),
            new(2500, "Small Celestial Emissary", false, false, new[] { "Central Yharnam", "Upper Cathedral Ward", "Forbidden Woods" }),
            new(2520, "Celestial Larva", false, false, new[] { "Upper Cathedral Ward" }),
            new(2530, "Lumenflower", false, false, new[] { "Byrgenwerth" }),
            new(2540, "Large Crawler", false, false, new[] { "Nightmare Frontier" }),
            new(2550, "Crawler", false, false, new[] { "Nightmare Frontier" }),
            new(2560, "Winter Lantern", false, false, new[] { "Nightmare of Mensis", "Nightmare Frontier", "Fishing Hamlet" }),
            new(2600, "Huntsman", false, false, new[] { "Cathedral Ward", "Central Yharnam", "Forbidden Woods" }),
            new(2610, "Hemwick Grave Woman", false, false, new[] { "Hemwick Charnel Lane", "Yahar'gul, Unseen Village" }),
            new(2620, "Wheelchair Huntsman", false, false, new[] { "Hemwick Charnel Lane", "Cathedral Ward", "Central Yharnam", "Yahar'gul, Unseen Village", "Research Hall" }),
            new(2630, "Large Huntsman", false, false, new[] { "Hemwick Charnel Lane", "Cathedral Ward", "Central Yharnam", "Yahar'gul, Unseen Village" }),
            new(2631, "Large Huntsman, torch and axe", false, false, new[] { "Cathedral Ward" }),
            new(2632, "Large Huntsman (Forbidden Woods)", false, false, new[] { "Forbidden Woods" }),
            new(2640, "Brick Troll", false, true, new[] { "Hemwick Charnel Lane", "Cathedral Ward", "Central Yharnam", "Yahar'gul, Unseen Village" }),
            new(2700, "Church Servant", false, true, new[] { "Cathedral Ward", "Upper Cathedral Ward", "Research Hall" }),
            new(2730, "Chapel Giant", false, true, new[] { "Cathedral Ward", "Forbidden Woods", "Yahar'gul, Unseen Village", "Byrgenwerth" }),
            new(2740, "Slime Scholar", false, false, new[] { "Byrgenwerth" }),
            new(3100, "Cramped Casket", false, false, new[] { "Yahar'gul, Unseen Village" }),
            new(4000, "Nightmare Hunter", false, true, new[] { "Hunter's Nightmare" }),
            new(4010, "Nightmare Executioner", false, true, new[] { "Hunter's Nightmare" }),
            new(4020, "Research Hall Patient", false, false, new[] { "Research Hall" }),
            new(4021, "Research Hall Patient, crawling", false, false, new[] { "Research Hall" }),
            new(4040, "Fishman", false, false, new[] { "Fishing Hamlet" }),
            new(4050, "Lightning Summoner", false, false, new[] { "Fishing Hamlet" }),
            new(4051, "Fishing Hamlet Priest", false, true, new[] { "Fishing Hamlet" }),
            new(4060, "Giant Fishman", false, true, new[] { "Fishing Hamlet" }),
            new(4070, "Orphan of Kos (Shell)", false, false, new[] { "Hunter's Nightmare", "Fishing Hamlet" }),
            new(4080, "Deep Sea Hound", false, false, new[] { "Fishing Hamlet" }),
            new(4100, "Rabid Dog (Hunter's Nightmare)", false, false, new[] { "Hunter's Nightmare" }),
            new(4110, "Large Huntsman (Hunter's Nightmare)", false, false, new[] { "Hunter's Nightmare" }),
            new(4120, "Enlarged Head Patient", false, true, new[] { "Research Hall" }),
            new(4130, "Bloodlicker (Hunter's Nightmare)", false, true, new[] { "Hunter's Nightmare" }),
            new(4140, "Cloaked Beast Patient", false, false, new[] { "Hunter's Nightmare" }),
            new(5033, "Snake", false, false, new[] { "Forbidden Woods" }),
            // the Chalice Dungeons' own kinds, placed only with option chalice_enemies (generator chalice_enemies.py)
            new(1010, "Loran Cleric", false, false, new[] { "Chalice Dungeons" }),
            new(1150, "Blood Gel", false, false, new[] { "Chalice Dungeons" }),
            new(1160, "Keeper's Hunting Dog", false, false, new[] { "Chalice Dungeons" }),
            new(1210, "Gravekeeper Scorpion", false, false, new[] { "Chalice Dungeons" }),
            new(1230, "Labyrinth Mole", false, false, new[] { "Chalice Dungeons" }),
            new(1300, "Nightmare Apostle", false, false, new[] { "Chalice Dungeons" }),
            new(2010, "Watcher's Gravedigger", false, false, new[] { "Chalice Dungeons" }),
            new(2040, "Skeletal Puppet", false, false, new[] { "Chalice Dungeons" }),
            new(2050, "Mad One", false, false, new[] { "Chalice Dungeons" }),
            new(2080, "Pilgrim", false, false, new[] { "Chalice Dungeons" }),
            new(2160, "Bone Ash Hunter", false, false, new[] { "Chalice Dungeons" }),
            new(3000, "Labyrinth Ritekeeper", false, false, new[] { "Chalice Dungeons" }),
            new(3010, "Pthumerian Undead", false, false, new[] { "Chalice Dungeons" }),
            new(3020, "Tomb Guardian", false, false, new[] { "Chalice Dungeons" }),
            new(3030, "Mad One", false, false, new[] { "Chalice Dungeons" }),
            new(3040, "Merciless Watcher", false, false, new[] { "Chalice Dungeons" }),
            new(3120, "Evil Labyrinth Spirit", false, false, new[] { "Chalice Dungeons" }),
        };
    }
}
