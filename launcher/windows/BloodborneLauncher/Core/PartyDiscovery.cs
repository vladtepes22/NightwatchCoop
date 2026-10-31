using System.Net;
using System.Net.Sockets;

namespace BloodborneLauncher.Core;

/// <summary>A party a host announced (the game's engine/gpu/shim/net/party_discovery.h, version 1).</summary>
public sealed record PartyAnnounce(uint Id, int Port, int Level, int Players, int Max, bool Password, bool Open, string Build, string Name, string Area);

/// <summary>A party heard on the network: the announcement and the address of the host it came from.</summary>
public sealed record FoundParty(PartyAnnounce Info, string Ip, long SeenMs)
{
    public string Address => NetworkRoute.HostAddress(Ip, Info.Port);
}

/// <summary>
/// Party discovery for the Join tab. A hosting game announces itself by UDP broadcast on 9308 (name, level, players, area,
/// build, whether it has a password; never the password, a code or an address). The launcher listens only while the Join tab
/// is open and reads the host's address from the datagram's source, so no netcode or relay lives here.
/// </summary>
public static class PartyDiscovery
{
    public const int Port = 9308;
    private static readonly System.Diagnostics.Stopwatch Clock = System.Diagnostics.Stopwatch.StartNew();
    /// <summary>The time base of the Book's entries (milliseconds since the launcher started).</summary>
    public static long NowMs => Clock.ElapsedMilliseconds;
    public const int MaxPacket = 160;
    public const int MaxBuild = 64, MaxName = 16, MaxArea = 32;
    public const int TtlMs = 6000;          // an announcement older than this is gone from the list
    public const int MaxEntries = 64;       // a flood of ids cannot grow the list further
    public const int MaxPerSecond = 20;     // packets per source address per second, the rest is dropped

    /// <summary>Strict, as the game's parse(): any wrong magic or version, an unknown flag bit, a short or long packet, a
    /// length that overruns, trailing bytes, a non-printable character, port 0, no or more than 8 slots, more players than
    /// slots, or an empty name returns null. Big endian: fa 'A' ver flags, id u32, port u16, level u16, players u8, max u8,
    /// then build, name and area as a length byte and that many printable ASCII bytes.</summary>
    public static PartyAnnounce? Parse(ReadOnlySpan<byte> d)
    {
        if (d.Length < 17 || d.Length > MaxPacket) return null;
        if (d[0] != 0xfa || d[1] != (byte)'A' || d[2] != 1 || (d[3] & ~3) != 0) return null;
        var id = (uint)d[4] << 24 | (uint)d[5] << 16 | (uint)d[6] << 8 | d[7];
        var port = d[8] << 8 | d[9];
        var level = d[10] << 8 | d[11];
        int players = d[12], max = d[13];
        if (port == 0 || max == 0 || max > 8 || players > max) return null;
        var pos = 14;
        if (!Text(d, ref pos, MaxBuild, out var build) || !Text(d, ref pos, MaxName, out var name) ||
            !Text(d, ref pos, MaxArea, out var area) || pos != d.Length || name.Length == 0)
            return null;
        return new PartyAnnounce(id, port, level, players, max, (d[3] & 1) != 0, (d[3] & 2) != 0, build, name, area);
    }

    private static bool Text(ReadOnlySpan<byte> d, ref int pos, int limit, out string text)
    {
        text = "";
        if (pos >= d.Length) return false;
        int n = d[pos++];
        if (n > limit || pos + n > d.Length) return false;
        foreach (var c in d.Slice(pos, n))
            if (c < 0x20 || c >= 0x7f) return false;
        text = System.Text.Encoding.ASCII.GetString(d.Slice(pos, n));
        pos += n;
        return true;
    }

    /// <summary>Does the host run this launcher's co-op build? The build id is "nightwatch-coop-&lt;version&gt;-&lt;hash&gt;" with the
    /// beta in the version; the launcher knows the version, not the hash, so only the version is compared (the game checks
    /// the whole id on joining). A stable launcher's prefix does not match a beta of that version.</summary>
    public static bool BuildMatches(string hostBuild, string prefix) =>
        hostBuild.StartsWith(prefix, StringComparison.Ordinal) && !hostBuild.AsSpan(prefix.Length).StartsWith("beta.", StringComparison.Ordinal);

    /// <summary>This launcher's build id prefix: "nightwatch-coop-0.2.6-beta.3-".</summary>
    public static string BuildPrefix(string version) => $"nightwatch-coop-{version}-";

    public enum RowState { Join, Full, OtherBuild }

    /// <summary>What a listed party's Join button does: join, or show why not (full, another co-op version).</summary>
    public static RowState StateOf(PartyAnnounce info, string prefix) =>
        !BuildMatches(info.Build, prefix) ? RowState.OtherBuild : info.Players >= info.Max ? RowState.Full : RowState.Join;

    /// <summary>Is this the party the saved host code or address points at?</summary>
    public static bool Selected(FoundParty party, string savedCode) =>
        savedCode.Trim().Equals(party.Address, StringComparison.OrdinalIgnoreCase);

    /// <summary>The launcher's Party size choice for a host's announced limit; null when it has no such choice (keep yours).</summary>
    public static string? PartySize(int max) => max is >= 2 and <= 5 ? max.ToString() : null;

    /// <summary>How a heard party is reached: "Nightwatch Online" on the tunnel, "VPN" on another VPN's network, else "LAN".</summary>
    public static string Tag(FoundParty party, IEnumerable<NetAdapter> adapters)
    {
        var ip = NetworkRoute.Parse(party.Ip);
        if (VpnLobby.IsTunnelAddress(ip)) return "Nightwatch Online";
        var onNet = adapters.FirstOrDefault(a => a.Ip != 0 && a.Mask != 0 && (a.Ip & a.Mask) == (ip & a.Mask));
        return onNet != null && NetworkRoute.KindOf(onNet) != VpnKind.None ? "VPN" : "LAN";
    }

    /// <summary>The networks the list hears parties on ("LAN" for a plain local network), and whether a Tailscale
    /// adapter is up (its parties never show: discovery skips Tailscale, NetworkRoute.DiscoveryOk).</summary>
    public static (List<string> Networks, bool Tailscale) Networks(IEnumerable<NetAdapter> adapters)
    {
        var list = adapters.ToList();
        var nets = list.Where(NetworkRoute.DiscoveryOk)
            .Select(a => NetworkRoute.KindOf(a) == VpnKind.None ? "LAN" : NetworkRoute.Label(a)).Distinct().ToList();
        return (nets, list.Any(a => a.Ip != 0 && NetworkRoute.KindOf(a) == VpnKind.Tailscale));
    }

    /// <summary>The announcements heard. One entry per game (the id), so a host announcing on several adapters is listed
    /// once; expires after <see cref="TtlMs"/>; at most <see cref="MaxEntries"/>; at most <see cref="MaxPerSecond"/> packets
    /// per source a second. Thread safe: the listener thread feeds it, the page reads snapshots.</summary>
    public sealed class Book
    {
        private readonly Dictionary<uint, FoundParty> _entries = new();
        private readonly Dictionary<string, (long Second, int Count)> _rate = new();
        private readonly HashSet<string> _own;
        private readonly object _lock = new();

        /// <param name="ownIps">This PC's own addresses: a host's own announcements are not listed.</param>
        public Book(IEnumerable<string>? ownIps = null) => _own = new HashSet<string>(ownIps ?? Array.Empty<string>());

        /// <summary>True when the packet was a valid announcement and went into the list.</summary>
        public bool Feed(ReadOnlySpan<byte> data, string ip, long nowMs)
        {
            if (_own.Contains(ip)) return false;
            lock (_lock)
            {
                var second = nowMs / 1000;
                var (sec, count) = _rate.TryGetValue(ip, out var r) && r.Second == second ? r : (second, 0);
                _rate[ip] = (sec, count + 1);
                if (_rate.Count > 4 * MaxEntries)
                    foreach (var key in _rate.Where(k => k.Value.Second < second - 1).Select(k => k.Key).ToList()) _rate.Remove(key);
                if (count >= MaxPerSecond) return false;
                if (Parse(data) is not { } info) return false;
                if (!_entries.ContainsKey(info.Id) && _entries.Count >= MaxEntries)
                {
                    Expire(nowMs);
                    if (_entries.Count >= MaxEntries) return false;
                }
                _entries[info.Id] = new FoundParty(info, ip, nowMs);
                return true;
            }
        }

        private void Expire(long nowMs)
        {
            foreach (var id in _entries.Where(e => nowMs - e.Value.SeenMs > TtlMs).Select(e => e.Key).ToList())
                _entries.Remove(id);
        }

        /// <summary>The parties still heard, by name.</summary>
        public List<FoundParty> Snapshot(long nowMs)
        {
            lock (_lock)
            {
                Expire(nowMs);
                return _entries.Values.OrderBy(e => e.Info.Name, StringComparer.OrdinalIgnoreCase).ThenBy(e => e.Ip, StringComparer.Ordinal).ToList();
            }
        }

        public void Clear() { lock (_lock) { _entries.Clear(); _rate.Clear(); } }
    }

    /// <summary>Listens on UDP <see cref="Port"/> (shared with other listeners) on its own thread and feeds a Book with
    /// the datagrams whose source is on a network the discovery rule allows (anySource: tests, loopback). Nothing here touches the UI thread.</summary>
    public sealed class Listener : IDisposable
    {
        private readonly Book _book;
        private readonly Func<IReadOnlyList<NetAdapter>> _adapters;
        private readonly int _port;
        private readonly bool _anySource;
        private readonly Thread _thread;
        private volatile bool _stop;
        /// <summary>Why listening failed (the port taken without sharing, a firewall), empty while it works.</summary>
        public volatile string Error = "";

        public Listener(Book book, Func<IReadOnlyList<NetAdapter>> adapters, int port = Port, bool anySource = false)
        {
            _anySource = anySource;
            _book = book;
            _adapters = adapters;
            _port = port;
            _thread = new Thread(Run) { IsBackground = true, Name = "party discovery" };
            _thread.Start();
        }

        private void Run()
        {
            Socket socket;
            try
            {
                socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                socket.Bind(new IPEndPoint(IPAddress.Any, _port));
                socket.ReceiveTimeout = 500;
            }
            catch (SocketException e) { Error = e.Message; return; }
            using (socket)
            {
                var buffer = new byte[512];
                EndPoint from = new IPEndPoint(IPAddress.Any, 0);
                while (!_stop)
                {
                    int n;
                    try { n = socket.ReceiveFrom(buffer, ref from); }
                    catch (SocketException e) when (e.SocketErrorCode is SocketError.TimedOut or SocketError.ConnectionReset) { continue; }
                    catch (Exception e) when (e is SocketException or ObjectDisposedException) { break; }
                    if (n <= 0 || from is not IPEndPoint remote) continue;
                    var ip = remote.Address.MapToIPv4().ToString();
                    if (!_anySource && !NetworkRoute.SourceAllowed(NetworkRoute.Parse(ip), _adapters())) continue;
                    _book.Feed(buffer.AsSpan(0, n), ip, NowMs);
                }
            }
        }

        public void Dispose() => _stop = true;  // the thread ends within the receive timeout
    }
}
