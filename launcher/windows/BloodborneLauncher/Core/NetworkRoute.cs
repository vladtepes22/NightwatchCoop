using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core;

/// <summary>An IPv4 address of an up network adapter, as the launcher shows it. The game picks its own adapter
/// (gpu/shim/net/net_socket.cpp); these rules repeat its choice so the launcher can show the address before the game
/// runs. Names follow the game's: the adapter's friendly name, " / ", its description.</summary>
public sealed record NetAdapter(string Name, uint Ip, uint Mask = 0, bool Loopback = false, bool Tunnel = false,
    bool Gateway = false, bool Physical = false, string Vendor = "", long[]? Rx1 = null, long[]? Rx2 = null)
{
    public string IpText => NetworkRoute.Text(Ip);
}

public enum VpnKind { None, Radmin, Hamachi, ZeroTier, Tailscale, Other }
public enum HostOver { Automatic, Vpn, Internet }

/// <summary>What the Co-op page shows as the party connection: the route (a VPN, the LAN or the Internet), its state and
/// the address to give friends. Addresses are shown on the user's own screen only; nothing here logs or sends them.</summary>
public sealed record Route(string Label, bool Connected, string Address, bool Vpn)
{
    public static readonly Route None = new("", false, "", false);
}

public static class NetworkRoute
{
    public const int DefaultPort = 9307;

    // Addresses are host-order uint32 (a.b.c.d = a << 24 | ...).
    public static uint Parse(string text)
    {
        if (!IPAddress.TryParse(text, out var ip) || ip.AddressFamily != AddressFamily.InterNetwork) return 0;
        var b = ip.GetAddressBytes();
        return (uint)b[0] << 24 | (uint)b[1] << 16 | (uint)b[2] << 8 | b[3];
    }

    public static string Text(uint ip) => $"{ip >> 24}.{(ip >> 16) & 255}.{(ip >> 8) & 255}.{ip & 255}";

    public static bool IsPrivate(uint ip) => ip >> 24 == 10 || ip >> 20 == 0xac1 || ip >> 16 == 0xc0a8;

    private static readonly string[] VirtualMarks =
    {
        "hyper-v", "vethernet", "wsl", "virtualbox", "vbox", "vmware", "vmnet", "virtual", "docker",
        "tap-windows", "tap adapter", "tap-", "openvpn", "wintun", "wireguard", "tailscale", "zerotier",
        "hamachi", "radmin", "nordlynx", "vpn", "anyconnect", "fortinet", "pangp", "loopback", "bluetooth",
    };

    /// <summary>Hyper-V / WSL / Docker switches, host-only adapters and VPN or overlay adapters, by name.</summary>
    public static bool IsVirtual(string name)
    {
        var n = name.ToLowerInvariant();
        return VirtualMarks.Any(n.Contains);
    }

    public static VpnKind KindOf(NetAdapter a)
    {
        var n = a.Name.ToLowerInvariant();
        if (n.Contains("radmin")) return VpnKind.Radmin;
        if (n.Contains("hamachi")) return VpnKind.Hamachi;
        if (n.Contains("zerotier")) return VpnKind.ZeroTier;
        if (n.Contains("tailscale")) return VpnKind.Tailscale;
        if (!IsVirtual(a.Name)) return VpnKind.None;
        if (a.Ip >> 24 == 26) return VpnKind.Radmin;
        if (a.Ip >> 24 == 25) return VpnKind.Hamachi;
        if (a.Ip >> 22 == 0x64400000u >> 22) return VpnKind.Tailscale;  // 100.64.0.0/10
        return new[] { "vpn", "tap", "wintun", "wireguard", "nordlynx", "anyconnect", "fortinet", "pangp" }.Any(n.Contains)
            ? VpnKind.Other : VpnKind.None;
    }

    /// <summary>"Radmin VPN", "Hamachi", "ZeroTier", "Tailscale"; another VPN: the adapter's friendly name.</summary>
    public static string Label(NetAdapter a) => KindOf(a) switch
    {
        VpnKind.Radmin => "Radmin VPN",
        VpnKind.Hamachi => "Hamachi",
        VpnKind.ZeroTier => "ZeroTier",
        VpnKind.Tailscale => "Tailscale",
        _ => a.Name.Contains(" / ") ? a.Name[..a.Name.IndexOf(" / ", StringComparison.Ordinal)] : a.Name,
    };

    /// <summary>party_vpn: "auto" (also empty), "off", or an adapter name (part of it, any case).</summary>
    public static string Normalize(string? pref)
    {
        var t = (pref ?? "").Trim();
        var l = t.ToLowerInvariant();
        if (l is "" or "auto" or "on" or "1") return "auto";
        if (l is "off" or "0" or "none" or "no") return "off";
        return t;
    }

    /// <summary>party_vpn -> segment: auto -> Automatic, off -> Internet, an adapter name -> Vpn.</summary>
    public static HostOver HostOverOf(string? pref) => Normalize(pref) switch
    {
        "auto" => HostOver.Automatic,
        "off" => HostOver.Internet,
        _ => HostOver.Vpn,
    };

    /// <summary>Usable VPN labels, distinct and best first as in Auto: Radmin, Hamachi, Tailscale, ZeroTier.</summary>
    public static List<string> VpnLabels(IEnumerable<NetAdapter> adapters) =>
        adapters.Where(Usable).Where(a => KindOf(a) is VpnKind.Radmin or VpnKind.Hamachi or VpnKind.Tailscale or VpnKind.ZeroTier)
            .OrderByDescending(a => KindOf(a) switch { VpnKind.Radmin => 4, VpnKind.Hamachi => 3, VpnKind.Tailscale => 2, _ => 1 })
            .Select(Label).Distinct().ToList();

    /// <summary>The segment's party_vpn value: auto, off, or the current adapter name, best VPN, then Radmin VPN.</summary>
    public static string PrefFor(HostOver choice, IEnumerable<NetAdapter> adapters, string? currentPref) => choice switch
    {
        HostOver.Automatic => "auto",
        HostOver.Internet => "off",
        _ => HostOverOf(currentPref) == HostOver.Vpn ? currentPref! : VpnLabels(adapters).FirstOrDefault() ?? "Radmin VPN",
    };

    /// <summary>A usable address: not loopback, a tunnel (Teredo, 6to4), 127/8 or 169.254/16.</summary>
    private static bool Usable(NetAdapter a) =>
        a.Ip != 0 && !a.Loopback && !a.Tunnel && a.Ip >> 24 != 127 && a.Ip >> 16 != 0xa9fe;

    public static string VpnStateFromRadminLog(string text)
    {
        var state = "";
        foreach (var line in text.Split('\n'))
        {
            if (line.Contains("Connected to server")) state = "online";
            else if (line.Contains("Disconnected from server") || line.Contains("Switched Off") || line.Contains("Service starting")) state = "offline";
        }
        return state;
    }

    public static string VpnStateFromTailscaleJson(string text)
    {
        var match = Regex.Match(text, "\"BackendState\"\\s*:\\s*\"([^\"]*)\"");
        return match.Groups[1].Value switch
        {
            "Running" => "online",
            "Stopped" or "NeedsLogin" or "NeedsMachineAuth" or "NoState" => "offline",
            _ => "",
        };
    }

    public static string VpnStateFromZerotierInfo(string text) => !text.Contains("200 info") ? "" :
        text.Contains("OFFLINE") ? "offline" : text.Contains("ONLINE") || text.Contains("TUNNELED") ? "online" : "";

    /// <summary>The app's own status wins. Quiet receive counters mean offline only for Automatic; without evidence,
    /// trust the adapter. A named preference trusts quiet counters, but cannot override the app's offline status.</summary>
    public static bool VpnDecideOnline(string vendor, long[]? rx1, long[]? rx2, bool countersTrusted = true)
    {
        if (vendor == "online") return true;
        if (vendor == "offline") return false;
        if (rx1 is not { Length: 3 } || rx2 is not { Length: 3 }) return true;
        return rx1.Zip(rx2).Any(pair => pair.Second > pair.First) || !countersTrusted;
    }

    private static bool IsOverlay(NetAdapter a) => KindOf(a) is VpnKind.Radmin or VpnKind.Hamachi or VpnKind.Tailscale or VpnKind.ZeroTier;

    public static bool AdapterOffline(NetAdapter a, string? pref = "auto") =>
        IsOverlay(a) && !VpnDecideOnline(a.Vendor, a.Rx1, a.Rx2, Normalize(pref) == "auto");

    public static List<string> OfflineVpnLabels(IEnumerable<NetAdapter> adapters, string? pref)
    {
        pref = Normalize(pref);
        return pref == "off" ? new() : adapters.Where(Usable).Where(a => AdapterOffline(a, pref) &&
            (pref == "auto" || a.Name.Contains(pref, StringComparison.OrdinalIgnoreCase))).Select(Label).ToList();
    }

    /// <summary>The adapter to host over for the preference, or null (off, or nothing matches). Auto takes the best of
    /// Radmin, Hamachi, Tailscale, ZeroTier; a commercial VPN is never taken unasked; a name takes the first adapter
    /// whose name contains it.</summary>
    public static NetAdapter? SelectVpn(IEnumerable<NetAdapter> adapters, string? pref)
    {
        pref = Normalize(pref);
        if (pref == "off") return null;
        NetAdapter? best = null;
        var bestScore = -1;
        foreach (var a in adapters)
        {
            if (!Usable(a) || AdapterOffline(a, pref)) continue;
            var score = pref == "auto"
                ? KindOf(a) switch { VpnKind.Radmin => 4, VpnKind.Hamachi => 3, VpnKind.Tailscale => 2, VpnKind.ZeroTier => 1, _ => -1 }
                : a.Name.Contains(pref, StringComparison.OrdinalIgnoreCase) ? 0 : -1;
            if (score > bestScore) { bestScore = score; best = a; }
        }
        return best;
    }

    /// <summary>The adapter the PC reaches the LAN or the Internet on: a real (non-virtual) one first, then one with a
    /// gateway, then a physical one.</summary>
    public static NetAdapter? SelectLan(IEnumerable<NetAdapter> adapters) =>
        adapters.Where(Usable).OrderByDescending(a => (IsVirtual(a.Name) ? 0 : 100) + (a.Gateway ? 4 : 0) + (a.Physical ? 2 : 0)).FirstOrDefault();

    public static string HostAddress(string ip, int port) => $"{ip}:{(port is >= 1 and <= 65535 ? port : DefaultPort)}";

    /// <summary>The route the party uses: the VPN adapter when party_vpn allows one, else the LAN adapter (a private
    /// address), else the Internet (a public address: friends use the party code), else nothing.</summary>
    public static Route Choose(IReadOnlyList<NetAdapter> adapters, string? vpnPref, int port)
    {
        if (SelectVpn(adapters, vpnPref) is { } vpn)
            return new Route(Label(vpn), true, HostAddress(vpn.IpText, port), true);
        if (SelectLan(adapters) is { } lan)
            return IsPrivate(lan.Ip)
                ? new Route("LAN", true, HostAddress(lan.IpText, port), false)
                : new Route("Internet", true, "", false);
        return Route.None;
    }

    /// <summary>May a party be announced or heard on this adapter (gpu/shim/net/party_discovery.h adapter_discovery_ok)? A
    /// private LAN address on a real adapter, or a Radmin (26/8) / Hamachi (25/8) / ZeroTier address. Never the Internet,
    /// Tailscale (no broadcasts), a commercial VPN or a virtual switch.</summary>
    public static bool DiscoveryOk(NetAdapter a)
    {
        if (a.Ip == 0 || a.Loopback || a.Tunnel) return false;
        var priv = IsPrivate(a.Ip);
        return KindOf(a) switch
        {
            VpnKind.Radmin => a.Ip >> 24 == 26 || priv,
            VpnKind.Hamachi => a.Ip >> 24 == 25 || priv,
            VpnKind.ZeroTier => priv,
            VpnKind.None => priv && !IsVirtual(a.Name),
            _ => false,
        };
    }

    /// <summary>Does a datagram from <paramref name="source"/> come from a network the discovery rule allows? Its address
    /// must lie in the subnet of an allowed adapter (a packet from the Internet or another network is dropped).</summary>
    public static bool SourceAllowed(uint source, IEnumerable<NetAdapter> adapters) =>
        adapters.Any(a => DiscoveryOk(a) && a.Mask != 0 && (a.Ip & a.Mask) == (source & a.Mask));

    private static async Task<string> RunVpnCli(string file, params string[] args)
    {
        using var process = new Process { StartInfo = new ProcessStartInfo(file)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
        } };
        foreach (var arg in args) process.StartInfo.ArgumentList.Add(arg);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(4));
        try
        {
            if (!process.Start()) return "";
            var output = process.StandardOutput.ReadToEndAsync(timeout.Token);
            var error = process.StandardError.ReadToEndAsync(timeout.Token);
            await Task.WhenAll(output, error, process.WaitForExitAsync(timeout.Token)).ConfigureAwait(false);
            return await output.ConfigureAwait(false);
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or IOException or InvalidOperationException or OperationCanceledException)
        {
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
            catch (Exception killError) when (killError is System.ComponentModel.Win32Exception or InvalidOperationException) { }
            return "";
        }
    }

    private static string VpnVendorState(VpnKind kind)
    {
        try
        {
            if (kind == VpnKind.Radmin)
            {
                var path = Path.Combine(Environment.GetEnvironmentVariable("ProgramData") ?? "", "Famatech", "Radmin VPN", "service.log");
                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                stream.Seek(Math.Max(0, stream.Length - 64 * 1024), SeekOrigin.Begin);
                var raw = new byte[64 * 1024];
                var count = stream.ReadAtLeast(raw, raw.Length, throwOnEndOfStream: false);
                // UTF-16 log: dropping zero bytes preserves ASCII events even if the tail starts mid-character.
                return VpnStateFromRadminLog(Encoding.UTF8.GetString(raw.Take(count).Where(b => b != 0).ToArray()));
            }
            if (kind == VpnKind.Tailscale)
            {
                var exe = Path.Combine(Environment.GetEnvironmentVariable("ProgramFiles") ?? "", "Tailscale", "tailscale.exe");
                return File.Exists(exe) ? VpnStateFromTailscaleJson(RunVpnCli(exe, "status", "--json").GetAwaiter().GetResult()) : "";
            }
            if (kind == VpnKind.ZeroTier)
            {
                var pf = Environment.GetEnvironmentVariable("ProgramFiles(x86)") ?? Environment.GetEnvironmentVariable("ProgramFiles") ?? "";
                var bat = Path.Combine(pf, "ZeroTier", "One", "zerotier-cli.bat");
                return File.Exists(bat) ? VpnStateFromZerotierInfo(RunVpnCli("cmd.exe", "/c", bat, "info").GetAwaiter().GetResult()) : "";
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return "";
    }

    private static long[]? ReceiveCounters(NetworkInterface nic)
    {
        try
        {
            var stats = nic.GetIPv4Statistics();
            return new[] { stats.BytesReceived, stats.UnicastPacketsReceived, stats.NonUnicastPacketsReceived };
        }
        catch (Exception e) when (e is NetworkInformationException or InvalidOperationException or NotSupportedException) { return null; }
    }

    /// <summary>The up IPv4 adapters and local VPN evidence. May wait 2 s for counters and up to 4 s per vendor CLI:
    /// call it off the UI thread.</summary>
    public static List<NetAdapter> Enumerate()
    {
        var found = new List<NetAdapter>();
        var overlays = new List<(int Index, NetworkInterface Nic)>();
        try
        {
            foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (nic.OperationalStatus != OperationalStatus.Up || nic.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                var props = nic.GetIPProperties();
                var gateway = props.GatewayAddresses.Any(g => g.Address.AddressFamily == AddressFamily.InterNetwork && !g.Address.Equals(IPAddress.Any));
                var physical = nic.NetworkInterfaceType is NetworkInterfaceType.Ethernet or NetworkInterfaceType.Wireless80211;
                foreach (var unicast in props.UnicastAddresses)
                {
                    if (unicast.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                    var ip = Parse(unicast.Address.ToString());
                    var mask = unicast.IPv4Mask is { } m ? Parse(m.ToString()) : 0;
                    found.Add(new NetAdapter(nic.Name + " / " + nic.Description, ip, mask, Tunnel: nic.NetworkInterfaceType == NetworkInterfaceType.Tunnel,
                        Gateway: gateway, Physical: physical));
                    if (IsOverlay(found[^1])) overlays.Add((found.Count - 1, nic));
                }
            }
        }
        catch (Exception e) when (e is NetworkInformationException or InvalidOperationException) { }
        if (Environment.GetEnvironmentVariable("BB_PARTY_VPN_PROBE") != "0")
        {
            var states = new Dictionary<VpnKind, string>();
            foreach (var (index, _) in overlays)
            {
                var kind = KindOf(found[index]);
                if (!states.TryGetValue(kind, out var state)) states[kind] = state = VpnVendorState(kind);
                found[index] = found[index] with { Vendor = state };
            }
        }
        if (overlays.Count > 0 && Environment.GetEnvironmentVariable("BB_VPN_RX") != "0")
        {
            var first = overlays.Select(a => ReceiveCounters(a.Nic)).ToArray();
            Thread.Sleep(TimeSpan.FromSeconds(2));
            for (var i = 0; i < overlays.Count; i++)
            {
                var (index, nic) = overlays[i];
                found[index] = found[index] with { Rx1 = first[i], Rx2 = ReceiveCounters(nic) };
            }
        }
        return found;
    }
}
