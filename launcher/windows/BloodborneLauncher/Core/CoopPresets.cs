namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The co-op setup wizard's table (Views/CoopWizard.cs): per page, the presets and the bbport.ini
    /// values each sets. The first choice of a page is today's shipped default, so a key missing from
    /// the ini counts as that choice's value. A page whose current values match no preset offers
    /// "Keep mine" and sets nothing.
    /// </summary>
    public static class CoopPresets
    {
        public sealed record Choice(string Name, string Info, (string Key, string Value)[] Values);

        public sealed record Page(string Title, string Intro, Choice[] Choices);

        /// <summary>Launcher setting: the wizard has been shown once, so it does not open on its own again.</summary>
        public const string DoneKey = "coop_wizard_done";

        public static Page[] Pages() => new[]
        {
            new Page(L.T("Guest health"), L.T("How much health a hunter has after joining a party."), new[]
            {
                new Choice(L.T("Cut by 30 %"), L.T("As in the game's own co-op: hunters who join have 30 % less maximum health."),
                    new[] { ("party_phantom_hp", "1") }),
                new Choice(L.T("Full health"), L.T("Guests keep their full maximum health."),
                    new[] { ("party_phantom_hp", "0") }),
            }),
            new Page(L.T("Dying and supplies"), L.T("What happens to a guest who dies, and whether their supplies come back."), new[]
            {
                new Choice(L.T("Vanilla"), L.T("A guest who dies comes back at the host's last lamp and is summoned again. Blood Vials and Quicksilver Bullets refill when the host rests, dies or travels."),
                    new[] { ("party_guest_respawn", "1"), ("party_guest_refill", "1") }),
                new Choice(L.T("Nightwatch"), L.T("A guest who dies is sent home, and supplies do not refill."),
                    new[] { ("party_guest_respawn", "0"), ("party_guest_refill", "0") }),
            }),
            new Page(L.T("Enemies stay dead"), L.T("Whether a kill counts for the rest of the party's visit."), new[]
            {
                new Choice(L.T("Vanilla"), L.T("A party kill stays dead for everyone, even when maps reload, until a rest."),
                    new[] { ("party_kill_record", "1") }),
                new Choice(L.T("Nightwatch"), L.T("Killed enemies come back when a map reloads."),
                    new[] { ("party_kill_record", "0") }),
            }),
            new Page(L.T("Toughness"), L.T("How much tougher enemies and bosses get for each guest."), new[]
            {
                new Choice(L.T("Vanilla"), L.T("The shipped values: boss HP +150 %, boss damage +10 %, enemy HP +50 % per guest. Already tougher than the game's own co-op."),
                    new[] { ("party_boss_hp", "150"), ("party_boss_damage", "10"), ("party_enemy_hp", "50"), ("party_enemy_damage", "0") }),
                new Choice(L.T("Nightwatch"), L.T("Boss HP +200 %, boss damage +20 %, enemy HP +100 %, enemy damage +10 % per guest. Poise stays as it is."),
                    new[] { ("party_boss_hp", "200"), ("party_boss_damage", "20"), ("party_enemy_hp", "100"), ("party_enemy_damage", "10") }),
            }),
            new Page(L.T("Rot on death"), L.T("Whether dying weakens you until you rest."), new[]
            {
                new Choice(L.T("Vanilla"), L.T("No penalty for dying."),
                    new[] { ("party_rot", "0") }),
                new Choice(L.T("Nightwatch"), L.T("Each death adds a penalty, stacking up to five times; resting at a lamp removes it."),
                    new[] { ("party_rot", "1") }),
            }),
        };

        /// <summary>The index of the choice whose values all equal the current ones (<paramref name="raw"/>
        /// gives a key's value in the ini, or null when absent), or -1 when none does.</summary>
        public static int Match(Page page, Func<string, string?> raw)
        {
            var defaults = page.Choices[0].Values.ToDictionary(v => v.Key, v => v.Value);
            for (var i = 0; i < page.Choices.Length; i++)
                if (page.Choices[i].Values.All(v => (raw(v.Key) ?? defaults[v.Key]) == v.Value))
                    return i;
            return -1;
        }
    }
}
