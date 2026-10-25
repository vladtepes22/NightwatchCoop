using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;

namespace BloodborneLauncher.Core;

/// <summary>An IPv4 address of an up network adapter, as the launcher shows it. The game picks its own adapter
/// (gpu/shim/net/net_socket.cpp); these rules repeat its choice so the launcher can show the address before the game
/// runs. Names follow the game's: the adapter's friendly name, " / ", its description.</summary>
public sealed record NetAdapter(string Name, uint Ip, uint Mask = 0, bool Loopback = false, bool Tunnel = false,
    bool Gateway = false, bool Physical = false)
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
            if (!Usable(a)) continue;
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

    /// <summary>The up adapters with an IPv4 address now. Blocks for a moment (the system call): call it off the UI thread.</summary>
    public static List<NetAdapter> Enumerate()
    {
        var found = new List<NetAdapter>();
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
                }
            }
        }
        catch (Exception e) when (e is NetworkInformationException or InvalidOperationException) { }
        return found;
    }
}
