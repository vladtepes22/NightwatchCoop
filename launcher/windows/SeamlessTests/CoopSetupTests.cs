using System.IO;
using BloodborneLauncher.Core;

/// <summary>Use recommended, and the settings folder's move from %APPDATA%\\bloodborne-seamless to nightwatch.</summary>
internal static class CoopSetupTests
{
    private static void Check(bool ok, string what)
    {
        if (!ok) throw new InvalidOperationException("Co-op setup: " + what);
    }

    private static void Recommended()
    {
        var set = new Dictionary<string, string>();
        CoopPresets.ApplyRecommended((k, v) => set[k] = v);
        var want = new Dictionary<string, string>
        {
            ["party_phantom_hp"] = "1", ["party_level_sync"] = "1", ["party_boss_drops"] = "1", ["party_travel_vote"] = "0", ["party_rot"] = "0",
            ["party_boss_hp"] = "150", ["party_boss_damage"] = "10", ["party_enemy_hp"] = "50", ["party_enemy_damage"] = "0",
        };
        Check(set.Count == want.Count && want.All(w => set[w.Key] == w.Value), "Use recommended is not exactly the shipped defaults with Seamless scaling");
        foreach (var page in CoopPresets.Pages())
            Check(CoopPresets.Match(page, k => set[k]) == CoopPresets.Match(page, _ => null), "A page does not read back its default: " + page.Title);
    }

    private static void Move()
    {
        var app = Path.Combine(Path.GetTempPath(), "nw-config-" + Guid.NewGuid().ToString("N"));
        string old = Path.Combine(app, "bloodborne-seamless"), now = Path.Combine(app, "nightwatch");
        var settings = Path.Combine("sub", "settings.json");
        void Fill(string dir, string text)
        {
            Directory.CreateDirectory(Path.Combine(dir, "sub"));
            File.WriteAllText(Path.Combine(dir, settings), text);
        }
        (string Dir, ConfigMove Move) Resolve(bool others = false) => Paths.ResolveConfig(app, () => others, true);
        try
        {
            Directory.CreateDirectory(app);
            var fresh = Resolve();
            Check(fresh == (now, ConfigMove.None) && !Directory.Exists(old), "Fresh install");

            Fill(old, "mine");
            var moved = Resolve();
            Check(moved == (now, ConfigMove.None) && !Directory.Exists(old) && File.ReadAllText(Path.Combine(now, settings)) == "mine", "Old only moves with its contents");
            Check(Resolve() == (now, ConfigMove.None), "A later start does nothing");

            Fill(old, "older");
            Check(Resolve() == (now, ConfigMove.OldKept), "Both: the new one wins");
            Check(File.ReadAllText(Path.Combine(old, settings)) == "older" && File.ReadAllText(Path.Combine(now, settings)) == "mine", "Both are left as they are");

            Directory.Delete(now, true);
            Check(Paths.ResolveConfig(app, () => false, false) == (old, ConfigMove.Failed) && !Directory.Exists(now), "A program other than the launcher moved the folder");
            Check(Resolve(others: true) == (old, ConfigMove.Failed) && !Directory.Exists(now), "Another launcher running: the old folder stays in use");
            using (new FileStream(Path.Combine(old, settings), FileMode.Open, FileAccess.Read, FileShare.None))
                Check(Resolve() == (old, ConfigMove.Failed) && !Directory.Exists(now), "A locked folder stays whole and in use");
            Check(File.ReadAllText(Path.Combine(old, settings)) == "older", "A failed move changed the contents");
            Check(Resolve() == (now, ConfigMove.None) && !Directory.Exists(old), "The next start moves it");

            // Concurrent starts: every launcher ends up on the one folder, none lost anything.
            Directory.Move(now, old);
            var results = new (string Dir, ConfigMove Move)[4];
            Parallel.For(0, results.Length, i => results[i] = Resolve());
            Check(results.All(r => r == (now, ConfigMove.None)) && File.ReadAllText(Path.Combine(now, settings)) == "older", "Concurrent starts");
        }
        finally { Directory.Delete(app, true); }
    }

    private static void HostModes()
    {
        foreach (var isPublic in new[] { false, true })
        foreach (var show in new[] { false, true })
        foreach (var pref in new[] { "off", "", "auto", "Example VPN" })
        {
            var writes = new Dictionary<string, string>();
            CoopHostMode.Apply(isPublic, show, pref, (k, v) => writes.Add(k, v));
            Check(writes.Count == 4, "Host mode writes exactly four settings");
            Check(writes["party_vpn_lobby"] == (isPublic && show ? "1" : "0"), "Lobby follows public visibility");
            Check(writes["party_discovery"] == (show ? "1" : "0") && writes["party_open"] == (show ? "1" : "0"), "Discovery and open follow Show");
            Check(writes["party_vpn"] == (isPublic || pref is "off" or "" ? "auto" : pref), "Private keeps a named adapter; public uses auto");
        }
        Check(CoopHostMode.MatchesSearch("Example Party", " pArT "), "Search ignores case and outer spaces");
        Check(CoopHostMode.MatchesSearch("Example Party", ""), "Empty search includes every party");
        Check(!CoopHostMode.MatchesSearch("Example Party", "absent"), "Search rejects nonmatches");
        var lan = new NetAdapter("Ethernet", NetworkRoute.Parse("192.168.50.2"), Gateway: true, Physical: true);
        Check(!NetworkRoute.Choose(new[] { lan }, "auto", 9307).Vpn, "Automatic without VPN uses LAN");
        foreach (var (name, kind) in new[] { ("Radmin VPN", VpnKind.Radmin), ("Hamachi", VpnKind.Hamachi),
            ("ZeroTier", VpnKind.ZeroTier), ("Tailscale", VpnKind.Tailscale), ("Example VPN", VpnKind.Other) })
        {
            var vpn = new NetAdapter(name, NetworkRoute.Parse("10.20.30.2"), Vendor: "online");
            Check(NetworkRoute.KindOf(vpn) == kind, "VPN fixture classification");
            var route = NetworkRoute.Choose(new[] { lan, vpn }, "auto", 9307);
            // Other VPNs are deliberately excluded by NetworkRoute's existing Automatic policy.
            Check(kind == VpnKind.Other ? !route.Vpn : route.Vpn && route.Address == "10.20.30.2:9307", "Automatic VPN policy: " + kind);
            Check(NetworkRoute.Choose(new[] { lan }, "auto", 9307).Address == "192.168.50.2:9307", "Missing VPN falls back to LAN");
        }
    }

    public static void Run()
    {
        HostModes();
        Recommended();
        Move();
        Console.WriteLine("PASS: Host modes, search, automatic routes; Use recommended; settings folder move");
    }
}
