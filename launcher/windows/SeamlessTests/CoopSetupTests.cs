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

    private static void Troubleshooter()
    {
        const string exe = @"C:\Game\bb-probe.exe";
        FirewallRuleInfo Rule(bool allow, string program, int protocol, string ports, bool inbound = true, bool enabled = true) => new(inbound, allow, enabled, program, protocol, ports);
        var full = new[] { Rule(true, exe, 6, "9307"), Rule(true, exe, 17, "9307") };
        Check(ConnectionCheck.ReadRules(full, exe, 9307) == (false, false), "Both allows are enough");
        Check(ConnectionCheck.ReadRules(new[] { Rule(true, exe, 6, "9307") }, exe, 9307).AllowMissing, "UDP allow is missing");
        Check(!ConnectionCheck.ReadRules(new[] { Rule(true, "", 256, "*") }, exe, 9307).AllowMissing, "An allow for all counts");
        Check(!ConnectionCheck.ReadRules(new[] { Rule(true, "", 6, "9000-9400"), Rule(true, "", 17, "80,9307") }, exe, 9307).AllowMissing, "Port lists and ranges");
        Check(ConnectionCheck.ReadRules(new[] { Rule(true, @"C:\Other\x.exe", 256, "*") }, exe, 9307).AllowMissing, "Another program's allow does not count");
        Check(ConnectionCheck.ReadRules(new[] { Rule(false, exe.ToUpperInvariant(), 256, "*") }, exe, 9307).Block, "A block of the game");
        Check(!ConnectionCheck.ReadRules(new[] { Rule(false, exe, 256, "*", enabled: false), Rule(false, exe, 256, "*", inbound: false) }, exe, 9307).Block, "Off and outbound blocks do not count");
        Check(!ConnectionCheck.ReadRules(new[] { Rule(false, @"C:\Other\x.exe", 256, "*") }, exe, 9307).Block, "A block of another program does not count");

        const string netstat = "  Proto  Local  Remote  State  PID\r\n  TCP  0.0.0.0:19307  0.0.0.0:0  ABHOEREN  1\r\n  TCP  10.0.0.2:5000  1.2.3.4:9307  HERGESTELLT  2\r\n  TCP  0.0.0.0:9307  0.0.0.0:0  ABHOEREN  77\r\n  UDP  0.0.0.0:9307  *:*  88\r\n";
        Check(ConnectionCheck.PortHolderPid(netstat, 9307) == 77, "netstat: the listener on the port, in any language");
        Check(ConnectionCheck.PortHolderPid("  UDP  0.0.0.0:9307  *:*  88\r\n", 9307) == 88, "netstat: UDP");
        Check(ConnectionCheck.PortHolderPid(netstat, 1234) == null, "netstat: nobody");

        Check(ConnectionCheck.IsCarrierNat("100.64.0.1") && ConnectionCheck.IsCarrierNat("100.127.255.1") && !ConnectionCheck.IsCarrierNat("100.128.0.1") && !ConnectionCheck.IsCarrierNat("192.168.1.2") && !ConnectionCheck.IsCarrierNat(""), "CGNAT range");

        string[] Ids(CheckFacts f) => ConnectionCheck.Detect(f).Select(p => p.Id).ToArray();
        var host = new CheckFacts { Hosting = true };
        Check(Ids(host).Length == 0, "Nothing known is no problem");
        Check(Ids(new CheckFacts { FirewallBlock = true, PortBusy = true }).Length == 0, "Firewall and port matter only to a host");
        Check(ConnectionCheck.Detect(host with { FirewallBlock = true })[0].Fix == CheckFix.Firewall, "Firewall block has a fix");
        Check(ConnectionCheck.Detect(host with { FirewallAllowMissing = true })[0].Fix == CheckFix.Firewall, "Missing allow has a fix");
        var port = ConnectionCheck.Detect(host with { PortBusy = true, PortHolder = "other", NextFreePort = 9310 })[0];
        Check(port.Fix == CheckFix.NextPort && port.Text.Contains("other") && port.Text.Contains("9310"), "Port: names the program and the next port");
        Check(ConnectionCheck.Detect(host with { PortBusy = true })[0].Fix == CheckFix.None, "Port: no fix without a free port");
        Check(Ids(new CheckFacts { Internet = false }).Contains("internet"), "No internet");
        Check(Ids(new CheckFacts { Internet = null }).Length == 0, "Unknown internet is no problem");
        Check(Ids(host with { LocalAddress = "100.70.0.1" }).Contains("nat") && !Ids(host with { LocalAddress = "100.70.0.1", PublicParty = true }).Contains("nat"), "CGNAT only for a private host");
        var two = new[] { "A", "B" };
        Check(Ids(host with { VpnsUp = two }).Contains("vpns") && !Ids(host with { VpnsUp = two, AutomaticRoute = false }).Contains("vpns") && !Ids(host with { VpnsUp = new[] { "A" } }).Contains("vpns"), "Several VPNs");
        Check(Ids(new CheckFacts { VpnNote = "wait" }).Contains("vpn"), "A VPN failure's message");
        Check(Ids(new CheckFacts { ClockSkew = TimeSpan.FromHours(-2) }).Contains("clock") && !Ids(new CheckFacts { ClockSkew = TimeSpan.FromMinutes(5) }).Contains("clock"), "Clock");
        Check(ConnectionCheck.Detect(new CheckFacts { MissingFiles = new[] { "bb-probe.exe" } })[0].Text.StartsWith("bb-probe.exe"), "Missing file is named");

        var lines = FirewallFix.Script(9307, exe, @"C:\Game\Launcher.exe").Split('\n');
        Check(lines.Length == 6 && lines[0] == FirewallFix.UnblockLine(exe) && lines[1] == FirewallFix.UnblockLine(@"C:\Game\Launcher.exe"), "Firewall: both programs are unblocked first");
        Check(lines.Skip(2).All(l => l.Contains("-Program '") && l.Contains("-Action Allow")), "Firewall: every allow is scoped to a program");
        Check(lines[5].Contains("Launcher.exe") && lines[5].Contains("-LocalPort " + PartyDiscovery.Port) && !lines[2].Contains("Launcher.exe"), "Firewall: only discovery opens for the launcher");
    }

    public static void Run()
    {
        Troubleshooter();
        HostModes();
        Recommended();
        Move();
        Console.WriteLine("PASS: Host modes, search, automatic routes; Use recommended; settings folder move");
    }
}
