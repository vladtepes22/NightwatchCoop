namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The co-op setup wizard's pages (Views/CoopWizard.cs): per page, the choices and the bbport.ini
    /// values each sets. The scaling page holds the Vanilla / Seamless / Nightwatch presets. A key missing
    /// from the ini counts as its shipped default (<see cref="Shipped"/>); a page whose current values match
    /// no choice offers "Keep mine" and sets nothing.
    /// </summary>
    public static class CoopPresets
    {
        public sealed record Choice(string Name, string Info, (string Key, string Value)[] Values);

        public sealed record Page(string Title, string Intro, Choice[] Choices);

        /// <summary>Launcher setting: the wizard has been shown once, so it does not open on its own again.</summary>
        public const string DoneKey = "coop_wizard_done";

        /// <summary>The values the engine uses when the ini has no entry (visy's, as shipped).</summary>
        private static readonly Dictionary<string, string> Shipped = new()
        {
            ["party_phantom_hp"] = "1", ["party_level_sync"] = "1", ["party_boss_drops"] = "1", ["party_travel_vote"] = "0",
            ["party_friendly_fire"] = "0", ["party_boss_hp"] = "150", ["party_boss_damage"] = "10", ["party_enemy_hp"] = "50", ["party_enemy_damage"] = "0",
            ["party_rot"] = "0",
        };

        private static (string, string)[] On(string key, bool on) => new[] { (key, on ? "1" : "0") };

        public static Page[] Pages() => new[]
        {
            new Page(L.T("Guest health"), L.T("How much health a hunter has after joining a party."), new[]
            {
                new Choice(L.T("Cut by 30 %"), L.T("As in the game's own co-op: hunters who join have 30 % less maximum health."), On("party_phantom_hp", true)),
                new Choice(L.T("Full health"), L.T("Guests keep their full maximum health."), On("party_phantom_hp", false)),
            }),
            new Page(L.T("Scaling presets"), L.T("How much tougher enemies and bosses get for each guest."), new[]
            {
                new Choice(L.T("Vanilla"),
                    L.T("The game's own numbers: boss health 100 %, no extra damage, no enemy scaling. The easy way to play seamless, for players who want a stress-free experience."),
                    new[] { ("party_boss_hp", "100"), ("party_boss_damage", "0"), ("party_enemy_hp", "0"), ("party_enemy_damage", "0") }),
                new Choice(L.T("Seamless"),
                    L.T("visy's numbers: boss health 150 %, boss damage +10 %, enemy health 50 %. The normal experience: easy for veterans, still enjoyable."),
                    new[] { ("party_boss_hp", "150"), ("party_boss_damage", "10"), ("party_enemy_hp", "50"), ("party_enemy_damage", "0") }),
                new Choice(L.T("Nightwatch (recommended)"),
                    L.T("Boss health 200 %, boss damage +20 %, enemy health 100 %, enemy damage +10 %. The recommended experience."),
                    new[] { ("party_boss_hp", "200"), ("party_boss_damage", "20"), ("party_enemy_hp", "100"), ("party_enemy_damage", "10") }),
            }),
            new Page(L.T("Rot on death"), L.T("Whether dying weakens you until you rest."), new[]
            {
                new Choice(L.T("Off"), L.T("No penalty for dying."), On("party_rot", false)),
                new Choice(L.T("On"), L.T("Each death adds a penalty, stacking up to five times; resting at a lamp removes it."), On("party_rot", true)),
            }),
            new Page(L.T("Level sync"), L.T("What happens when a guest's level is far above the host's."), new[]
            {
                new Choice(L.T("On"), L.T("As in the game's own co-op: a hunter whose level is above the host's range is scaled down while it plays in the host's world."), On("party_level_sync", true)),
                new Choice(L.T("Off"), L.T("Everyone keeps full stats."), On("party_level_sync", false)),
            }),
            new Page(L.T("Boss drops for guests"), L.T("Who gets what a world boss drops."), new[]
            {
                new Choice(L.T("On"), L.T("The item a world boss drops for the host also goes to every guest who is there, once, in their own save."), On("party_boss_drops", true)),
                new Choice(L.T("Off"), L.T("Only the host gets the drop."), On("party_boss_drops", false)),
            }),
            new Page(L.T("Travel by vote"), L.T("Who decides when the party travels by lamp, headstone or Hunter's Mark."), new[]
            {
                new Choice(L.T("Off"), L.T("Whoever travels takes the party along."), On("party_travel_vote", false)),
                new Choice(L.T("On"), L.T("Travel needs every member's approval; one refusal cancels it."), On("party_travel_vote", true)),
            }),
            new Page(L.T("Friendly fire / PvP"), L.T("Whether party members can hurt each other."), new[]
            {
                new Choice(L.T("Off"), L.T("Party members cannot hurt each other."), On("party_friendly_fire", false)),
                new Choice(L.T("On"), L.T("Party members can hurt each other; being killed by a party member counts as a normal death."), On("party_friendly_fire", true)),
            }),
        };

        /// <summary>The index of the choice whose values all equal the current ones (<paramref name="raw"/>
        /// gives a key's value in the ini, or null when absent), or -1 when none does.</summary>
        public static int Match(Page page, Func<string, string?> raw)
        {
            for (var i = 0; i < page.Choices.Length; i++)
                if (page.Choices[i].Values.All(v => (raw(v.Key) ?? Shipped[v.Key]) == v.Value))
                    return i;
            return -1;
        }
    }
}
