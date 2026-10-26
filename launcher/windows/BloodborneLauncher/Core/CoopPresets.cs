namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The co-op setup wizard's presets (Views/CoopWizard.cs): whole sets of bbport.ini rule values.
    /// A key missing from the ini counts as its shipped default (<see cref="Shipped"/>). Values that match
    /// no preset are offered as "Keep mine" and set nothing.
    /// </summary>
    public static class CoopPresets
    {
        public sealed record Preset(string Name, string Info, (string Key, string Value)[] Values);

        /// <summary>Launcher setting: the wizard has been shown once, so it does not open on its own again.</summary>
        public const string DoneKey = "coop_wizard_done";

        /// <summary>The values the engine uses when the ini has no entry (visy's, as shipped).</summary>
        private static readonly (string Key, string Value)[] Shipped =
        {
            ("party_phantom_hp", "1"), ("party_guest_respawn", "1"), ("party_guest_refill", "1"), ("party_kill_record", "1"),
            ("party_boss_hp", "150"), ("party_boss_damage", "10"), ("party_enemy_hp", "50"), ("party_enemy_damage", "0"),
            ("party_rot", "0"),
        };

        private static (string Key, string Value)[] With(params (string Key, string Value)[] changes) =>
            Shipped.Select(s => changes.FirstOrDefault(c => c.Key == s.Key) is { Key: not null } c ? c : s).ToArray();

        public static Preset[] Presets() => new[]
        {
            new Preset(L.T("Vanilla"),
                L.T("The game's own co-op numbers, with the seamless conveniences on. The easy way to play seamless, for players who want a stress-free experience."),
                With(("party_boss_hp", "100"), ("party_boss_damage", "0"), ("party_enemy_hp", "100"))),
            new Preset(L.T("Seamless"),
                L.T("The normal seamless experience: no phantom health cut and visy's scaling. Easy for veterans, still enjoyable."),
                With(("party_phantom_hp", "0"))),
            new Preset(L.T("Nightwatch (recommended)"),
                L.T("The recommended experience: phantom health cut, harder scaling, a guest who dies is sent home, enemies come back, rot on death."),
                With(("party_guest_respawn", "0"), ("party_kill_record", "0"),
                    ("party_boss_hp", "200"), ("party_boss_damage", "20"), ("party_enemy_hp", "100"), ("party_enemy_damage", "10"),
                    ("party_rot", "1"))),
        };

        /// <summary>The index of the preset whose values all equal the current ones (<paramref name="raw"/>
        /// gives a key's value in the ini, or null when absent), or -1 when none does.</summary>
        public static int Match(Preset[] presets, Func<string, string?> raw)
        {
            var defaults = Shipped.ToDictionary(s => s.Key, s => s.Value);
            for (var i = 0; i < presets.Length; i++)
                if (presets[i].Values.All(v => (raw(v.Key) ?? defaults[v.Key]) == v.Value))
                    return i;
            return -1;
        }
    }
}
