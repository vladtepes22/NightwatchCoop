using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace BloodborneLauncher.Core;

/// <summary>A game in the Nightwatch VPN lobby. The address is for the join alone: not a record, so nothing prints it,
/// and it is never shown, copied, saved or logged.</summary>
public sealed class LobbyGame
{
    public required string Name { get; init; }
    public required int Players { get; init; }
    public required int Max { get; init; }
    public required bool Password { get; init; }
    public required string Build { get; init; }
    public required bool Own { get; init; }
    public required uint Ip { get; init; }
    public required int Port { get; init; }
    public string JoinCode => NetworkRoute.HostAddress(NetworkRoute.Text(Ip), Port);
    public override string ToString() => Name;
}

/// <summary>
/// The list side of the lobby on the Nightwatch VPN server (engine/docs/party/nightwatch_vpn.md, game side
/// net/party_vpnlobby.h). LAN discovery does not cross an OpenVPN tunnel, so hosting games announce to this lobby, and only
/// while this PC has a tunnel address and the player has the lobby on does the launcher ask for the list.
/// </summary>
public static class VpnLobby
{
    public const int Port = 9310, PollMs = 5000, TimeoutMs = 1000, MaxPages = 20;
    public const string Address = "10.8.0.1";
    private const int FlagPassword = 1, FlagOwn = 2;

    /// <summary>A player address of the tunnel: 10.8.0.2 to 10.8.0.254.</summary>
    public static bool IsTunnelAddress(uint ip) => (ip & 0xffffff00) == 0x0a080000 && (ip & 0xff) is >= 2 and <= 254;

    /// <summary>This PC's tunnel address, or null while the VPN is off.</summary>
    public static string? TunnelIp(IEnumerable<NetAdapter> adapters) =>
        adapters.FirstOrDefault(a => IsTunnelAddress(a.Ip))?.IpText;

    /// <summary>BB_PARTY_VPNLOBBY_ADDR=ip:port (tests): another lobby, no tunnel needed; as in the game.</summary>
    public static IPEndPoint? TestTarget() =>
        Environment.GetEnvironmentVariable("BB_PARTY_VPNLOBBY_ADDR") is { Length: > 0 } text && IPEndPoint.TryParse(text, out var target) ? target : null;

    public static byte[] ListRequest(int offset) => new byte[] { 0xfa, (byte)'L', 1, 2, (byte)(offset >> 8), (byte)offset };

    /// <summary>Strict, untrusted input: anything malformed gives null. Reply: fa 'L' 1 3, total u16, offset u16, count u8, then
    /// ipv4, port u16, flags u8, players u8, max u8, name (u8 len + UTF-8), build (u8 len + printable ASCII).</summary>
    public static (int Total, int Offset, List<LobbyGame> Games)? ParseReply(ReadOnlySpan<byte> d)
    {
        if (d.Length is < 9 or > 1024 || d[0] != 0xfa || d[1] != (byte)'L' || d[2] != 1 || d[3] != 3) return null;
        int total = d[4] << 8 | d[5], offset = d[6] << 8 | d[7], count = d[8], pos = 9;
        var games = new List<LobbyGame>();
        var utf8 = new UTF8Encoding(false, throwOnInvalidBytes: true);
        for (var i = 0; i < count; i++)
        {
            if (pos + 10 > d.Length) return null;
            var ip = (uint)d[pos] << 24 | (uint)d[pos + 1] << 16 | (uint)d[pos + 2] << 8 | d[pos + 3];
            var port = d[pos + 4] << 8 | d[pos + 5];
            int flags = d[pos + 6], players = d[pos + 7], max = d[pos + 8], n = d[pos + 9];
            pos += 10;
            if ((flags & ~(FlagPassword | FlagOwn)) != 0 || port < 1024 || max is < 1 or > 8 || players > max || n == 0 || pos + n + 1 > d.Length) return null;
            string name;
            try { name = CleanName(utf8.GetString(d.Slice(pos, n))); }
            catch (ArgumentException) { return null; }
            pos += n;
            int b = d[pos++];
            if (pos + b > d.Length || name.Length == 0) return null;
            foreach (var c in d.Slice(pos, b))
                if (c is < 0x20 or >= 0x7f) return null;
            var build = Encoding.ASCII.GetString(d.Slice(pos, b));
            pos += b;
            games.Add(new LobbyGame
            {
                Name = name, Players = players, Max = max, Password = (flags & FlagPassword) != 0, Own = (flags & FlagOwn) != 0,
                Build = build, Ip = ip, Port = port,
            });
        }
        return pos == d.Length ? (total, offset, games) : null;
    }

    /// <summary>A hostile lobby cannot put control, format or invisible characters into the page: printable ones, at most 16.</summary>
    private static string CleanName(string name)
    {
        var clean = new StringBuilder();
        var kept = 0;
        foreach (var rune in name.EnumerateRunes())
        {
            if (kept == 16) break;
            if (Rune.GetUnicodeCategory(rune) is UnicodeCategory.Control or UnicodeCategory.Format or UnicodeCategory.Surrogate or
                UnicodeCategory.PrivateUse or UnicodeCategory.OtherNotAssigned or UnicodeCategory.LineSeparator or UnicodeCategory.ParagraphSeparator) continue;
            if (Rune.GetUnicodeCategory(rune) == UnicodeCategory.SpaceSeparator && rune.Value != ' ') continue;
            clean.Append(rune.ToString());
            kept++;
        }
        return clean.ToString();
    }

    /// <summary>The rows to show, by name: your own game left out, and (unless testing) only tunnel addresses, so a lobby
    /// that lies cannot send a guest's password to somewhere outside the VPN. A game without a password is an open party
    /// for the tunnel (party_open_rules.h), so it is listed too.</summary>
    public static List<LobbyGame> Rows(IEnumerable<LobbyGame> games, bool anyAddress = false) =>
        games.Where(g => !g.Own && (anyAddress || IsTunnelAddress(g.Ip)))
            .OrderBy(g => g.Name, StringComparer.OrdinalIgnoreCase).ThenBy(g => g.Ip).ToList();

    /// <summary>The version in a build id, for the list: "nightwatch-coop-0.2.6-beta.3-a1b2c3" is "0.2.6 beta 3", and
    /// "nightwatch-coop-0.2.6-a1b2c3" (a stable build, or a beta before beta 3) is "0.2.6".</summary>
    public static string VersionOf(string build)
    {
        var match = System.Text.RegularExpressions.Regex.Match(build, @"^nightwatch-coop-(\d+\.\d+\.\d+)(?:-beta\.(\d+))?(?:-|$)");
        if (!match.Success) return build;
        return match.Groups[2].Success ? match.Groups[1].Value + " beta " + match.Groups[2].Value : match.Groups[1].Value;
    }

    /// <summary>All pages of the list, or null when the lobby does not answer. The connected socket hears only the lobby.</summary>
    public static List<LobbyGame>? Fetch(IPEndPoint target, string? bindIp, int timeoutMs = TimeoutMs)
    {
        try
        {
            using var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp) { ReceiveTimeout = timeoutMs };
            if (bindIp != null) socket.Bind(new IPEndPoint(IPAddress.Parse(bindIp), 0));
            socket.Connect(target);
            var games = new List<LobbyGame>();
            var buffer = new byte[2048];
            var offset = 0;
            for (var page = 0; page < MaxPages; page++)
            {
                socket.Send(ListRequest(offset));
                var n = socket.Receive(buffer);
                if (ParseReply(buffer.AsSpan(0, n)) is not { } reply || reply.Offset != offset) return games.Count > 0 ? games : null;
                games.AddRange(reply.Games);
                offset += reply.Games.Count;
                if (reply.Games.Count == 0 || offset >= reply.Total) break;
            }
            return games;
        }
        catch (Exception e) when (e is SocketException or FormatException) { return null; }
    }

    /// <summary>Asks every 5 s on its own thread, only while <paramref name="source"/> says so (the lobby on and a tunnel
    /// address, or a test target); sends nothing otherwise. The page reads <see cref="Games"/>.</summary>
    public sealed class Poller : IDisposable
    {
        private readonly Func<(IPEndPoint Target, string? Bind)?> _source;
        private readonly ManualResetEventSlim _stop = new();
        public volatile List<LobbyGame>? Games;
        public volatile bool Answered;

        public Poller(Func<(IPEndPoint Target, string? Bind)?> source)
        {
            _source = source;
            new Thread(Run) { IsBackground = true, Name = "vpn lobby" }.Start();
        }

        private void Run()
        {
            do
            {
                if (_source() is { } to)
                {
                    var games = Fetch(to.Target, to.Bind);
                    Answered = games != null;
                    Games = games;
                }
                else
                {
                    Answered = false;
                    Games = null;
                }
            } while (!_stop.Wait(PollMs));
        }

        public void Dispose() => _stop.Set();
    }
}
