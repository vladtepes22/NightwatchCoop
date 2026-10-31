using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;

namespace BloodborneLauncher.Core;

public enum CheckFix { None, Firewall, NextPort }

public sealed record CheckProblem(string Id, string Text, CheckFix Fix);

public sealed record FirewallRuleInfo(bool Inbound, bool Allow, bool Enabled, string Program, int Protocol, string Ports);

/// <summary>What one run found out. A null is "could not tell": it never raises a problem.</summary>
public sealed record CheckFacts
{
    public bool Hosting { get; init; }
    public bool PublicParty { get; init; }
    public bool? FirewallBlock { get; init; }
    public bool? FirewallAllowMissing { get; init; }
    public bool PortBusy { get; init; }
    public string PortHolder { get; init; } = "";
    public int NextFreePort { get; init; }
    public bool? Internet { get; init; }
    public string LocalAddress { get; init; } = "";
    public IReadOnlyList<string> VpnsUp { get; init; } = Array.Empty<string>();
    public bool AutomaticRoute { get; init; } = true;
    public string VpnNote { get; init; } = "";
    public TimeSpan? ClockSkew { get; init; }
    public IReadOnlyList<string> MissingFiles { get; init; } = Array.Empty<string>();
}

/// <summary>The co-op tab's silent troubleshooter. <see cref="Detect"/> and the parsers are pure; <see cref="Gather"/>
/// reads this PC without admin rights, off the UI thread. A different build from the host is on the list rows ("Update
/// needed"), not here.</summary>
public static class ConnectionCheck
{
    private const int TcpProtocol = 6, UdpProtocol = 17, AnyProtocol = 256;
    private static readonly TimeSpan ClockLimit = TimeSpan.FromMinutes(10);

    public static List<CheckProblem> Detect(CheckFacts f)
    {
        var found = new List<CheckProblem>();
        void Add(string id, string text, CheckFix fix = CheckFix.None) => found.Add(new CheckProblem(id, text, fix));
        foreach (var file in f.MissingFiles)
            Add("files", file + L.T(" is missing. An antivirus may have quarantined it: restore it, or reinstall Nightwatch Co-op."));
        if (f.Hosting && f.FirewallBlock == true)
            Add("firewall", L.T("Windows Firewall blocks the game, so friends cannot reach your party."), CheckFix.Firewall);
        else if (f.Hosting && f.FirewallAllowMissing == true)
            Add("firewall", L.T("Windows Firewall has no rule for the party port, so friends may not reach your party."), CheckFix.Firewall);
        if (f.Hosting && f.PortBusy)
            Add("port", (f.PortHolder.Length > 0 ? f.PortHolder + L.T(" is using the party port.") : L.T("Another program is using the party port."))
                + (f.NextFreePort > 0 ? " " + L.T("Fix moves the party to port ") + f.NextFreePort + L.T("; friends then need your new code.") : ""),
                f.NextFreePort > 0 ? CheckFix.NextPort : CheckFix.None);
        if (f.Internet == false) Add("internet", L.T("This PC could not reach the internet. Friends over the internet and the Nightwatch VPN will not work."));
        if (f.Hosting && !f.PublicParty && IsCarrierNat(f.LocalAddress))
            Add("nat", L.T("Your provider shares one public address between customers (CGNAT), so friends may not reach you directly. Use Nightwatch Online, or port forwarding if your provider allows it (Advanced)."));
        if (f.Hosting && !f.PublicParty && f.AutomaticRoute && f.VpnsUp.Count > 1)
            Add("vpns", L.T("Several VPNs are up: ") + string.Join(", ", f.VpnsUp) + L.T(". Pick the one your friends are in under Advanced."));
        if (f.VpnNote.Length > 0) Add("vpn", f.VpnNote);
        if (f.ClockSkew is { } skew && skew.Duration() > ClockLimit)
            Add("clock", L.T("This PC's clock is wrong, so VPN certificates can fail. Set the date and time in Windows."));
        return found;
    }

    /// <summary>100.64.0.0/10, on the adapter that leads to the internet.</summary>
    public static bool IsCarrierNat(string address) =>
        IPAddress.TryParse(address, out var ip) && ip.AddressFamily == AddressFamily.InterNetwork
        && ip.GetAddressBytes() is [100, >= 64 and <= 127, ..];

    /// <summary>Blocks and allows of the game's inbound rules. An allow counts when it is for the game or for any program,
    /// and covers the port (a rule for all ports does).</summary>
    public static (bool Block, bool AllowMissing) ReadRules(IEnumerable<FirewallRuleInfo> rules, string exe, int port)
    {
        var mine = rules.Where(r => r.Inbound && r.Enabled).ToList();
        var ofGame = mine.Where(r => string.Equals(r.Program, exe, StringComparison.OrdinalIgnoreCase)).ToList();
        bool Allowed(int protocol) => mine.Any(r => r.Allow
            && (r.Program.Length == 0 || string.Equals(r.Program, exe, StringComparison.OrdinalIgnoreCase))
            && (r.Protocol == AnyProtocol || r.Protocol == protocol) && CoversPort(r.Ports, port));
        return (ofGame.Any(r => !r.Allow), !Allowed(TcpProtocol) || !Allowed(UdpProtocol));
    }

    private static bool CoversPort(string ports, int port) =>
        ports is "" or "*" || ports.Split(',').Any(part =>
        {
            var range = part.Trim().Split('-');
            return int.TryParse(range[0], out var low) && (range.Length == 1 ? low == port
                : int.TryParse(range[1], out var high) && port >= low && port <= high);
        });

    /// <summary>The process id holding the port in `netstat -ano` output. Only the columns are read: the state word is
    /// translated on non-English Windows.</summary>
    public static int? PortHolderPid(string netstat, int port)
    {
        foreach (var line in netstat.Split('\n'))
        {
            var parts = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 4 || parts[0] is not ("TCP" or "UDP") || !parts[1].EndsWith(":" + port)) continue;
            var listening = parts[0] == "UDP" || parts[2].EndsWith(":0");
            if (listening && int.TryParse(parts[^1], out var pid)) return pid;
        }
        return null;
    }

    private static bool PortFree(int port)
    {
        try
        {
            var tcp = new TcpListener(IPAddress.Any, port);
            tcp.Start();
            tcp.Stop();
            using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, port));
            return true;
        }
        catch (SocketException) { return false; }
    }

    private static string HolderName(int port)
    {
        try
        {
            var start = new ProcessStartInfo("netstat.exe", "-ano") { RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true };
            using var netstat = Process.Start(start);
            if (netstat == null) return "";
            var text = netstat.StandardOutput.ReadToEnd();
            netstat.WaitForExit(3000);
            if (PortHolderPid(text, port) is not { } pid) return "";
            using var holder = Process.GetProcessById(pid);
            return holder.ProcessName;
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or System.ComponentModel.Win32Exception or IOException) { return ""; }
    }

    private static (bool? Block, bool? AllowMissing) ReadFirewall(string exe, int port)
    {
        try
        {
            if (Type.GetTypeFromProgID("HNetCfg.FwPolicy2") is not { } type) return (null, null);
            dynamic policy = Activator.CreateInstance(type)!;
            var rules = new List<FirewallRuleInfo>();
            foreach (dynamic rule in policy.Rules)
                rules.Add(new FirewallRuleInfo((int)rule.Direction == 1, (int)rule.Action == 1, (bool)rule.Enabled,
                    (string?)rule.ApplicationName ?? "", (int)rule.Protocol, (string?)rule.LocalPorts ?? ""));
            var state = ReadRules(rules, exe, port);
            return (state.Block, state.AllowMissing);
        }
        catch (Exception e) when (e is System.Runtime.InteropServices.COMException or InvalidCastException or Microsoft.CSharp.RuntimeBinder.RuntimeBinderException or UnauthorizedAccessException) { return (null, null); }
    }

    /// <summary>Everything off the UI thread takes; <paramref name="start"/> holds what the page already knows.</summary>
    public static async Task<CheckFacts> Gather(CheckFacts start, int port, string stun, bool gameRunning)
    {
        var exe = Path.Combine(Paths.BinariesDir, Paths.Program("bb-probe"));
        var facts = start;
        var missing = Directory.Exists(Paths.BinariesDir)
            ? new[] { "bb-probe", "bb-start" }.Select(Paths.Program).Where(f => !File.Exists(Path.Combine(Paths.BinariesDir, f))).ToArray()
            : Array.Empty<string>();
        facts = facts with { MissingFiles = missing };
        if (start.Hosting)
        {
            var firewall = ReadFirewall(exe, port);
            facts = facts with { FirewallBlock = firewall.Block, FirewallAllowMissing = firewall.AllowMissing };
            if (!gameRunning && !PortFree(port))
            {
                var next = Enumerable.Range(port + 1, 20).FirstOrDefault(p => p <= 65535 && PortFree(p));
                facts = facts with { PortBusy = true, PortHolder = HolderName(port), NextFreePort = next };
            }
        }
        if (stun != "off" && stun != "0")
        {
            string? found = null;
            var answered = true;
            try { found = await PartyCodes.PublicAddressAsync("", stun).ConfigureAwait(false); }
            catch (Exception e) when (e is not OutOfMemoryException) { answered = false; }
            facts = facts with { Internet = answered ? found != null : null };
        }
        return facts;
    }
}
