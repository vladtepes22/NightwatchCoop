using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;

namespace BloodborneLauncher.Core;

/// <summary>The host's party codes before the game starts: the same BBP3 codes the engine writes to party_code.txt
/// (engine/gpu/shim/party/party_code.cpp: encode_v3, fit_secret_to_code), from the saved secret (party_secret.txt, the
/// file the engine reads at every launch), the party port, whether a password is set and an address: this PC's public IPv4
/// (STUN, or the Host public address) for the Internet code, the local network / VPN address for the LAN code. While the game
/// hosts, the engine's own file wins (it also knows UPnP and the IPv6 tail).</summary>
public static class PartyCodes
{
    public const string DefaultStun = "stun.l.google.com:19302";
    private const string Alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    private static readonly uint[] Polynomials = { 0x07, 0x119, 0x233, 0x385, 0x80F };  // check widths 8..12
    private const int SecretBytes = 6, SecretBytesWithPassword = 4;

    /// <summary>"BBP3-XXXX-XXXX-...". lan: only the address's last octet is carried (the guest takes the rest from its own network).</summary>
    public static string Encode(bool lan, bool password, byte[] ipv4, int port, byte[] secret)
    {
        var wire = (lan ? 1 : 0) | (password ? 2 : 0) | (port == NetworkRoute.DefaultPort ? 8 : 0);
        var bits = new List<byte>();
        Put(bits, (uint)wire, 4);
        if (lan) Put(bits, ipv4[3], 8);
        else foreach (var b in ipv4) Put(bits, b, 8);
        if ((wire & 8) == 0) Put(bits, (uint)port, 16);
        for (var i = 0; i < (password ? SecretBytesWithPassword : SecretBytes); i++) Put(bits, secret[i], 8);
        var payload = bits.Count;
        var symbols = (payload + 8 + 4) / 5;
        var k = symbols * 5 - payload;  // 8..12 check bits
        Put(bits, Crc(bits, k), k);
        var text = new System.Text.StringBuilder("BBP3");
        for (var i = 0; i < symbols; i++)
        {
            if (i % 4 == 0) text.Append('-');
            var value = 0;
            for (var b = 0; b < 5; b++) value = value << 1 | bits[i * 5 + b];
            text.Append(Alphabet[value]);
        }
        return text.ToString();
    }

    private static void Put(List<byte> bits, uint value, int count)
    {
        for (var i = count - 1; i >= 0; i--) bits.Add((byte)(value >> i & 1));
    }

    private static uint Crc(List<byte> bits, int k)
    {
        var poly = Polynomials[k - 8];
        var mask = (1u << k) - 1;
        var register = mask;
        foreach (var bit in bits)
        {
            var top = (register >> (k - 1) & 1) ^ bit;
            register = register << 1 & mask;
            if (top != 0) register ^= poly;
        }
        return register;
    }

    /// <summary>party_secret.txt: 16 hex digits, 8 bytes, not all zero. Null for anything else.</summary>
    public static byte[]? ParseSecret(string? text)
    {
        text = text?.Trim();
        if (text is not { Length: 16 }) return null;
        try
        {
            var secret = Convert.FromHexString(text);
            return secret.Any(b => b != 0) ? secret : null;
        }
        catch (FormatException) { return null; }
    }

    /// <summary>The secret as a code carries it: 4 bytes with a password (the password keeps strangers out), 6 without. A
    /// stored secret of the other size is shortened or lengthened and a missing one made, as the engine does at start-up
    /// (fit_secret_to_code), so the code shown here is the one the game writes. Null / invalid in: a new random secret.</summary>
    public static byte[] FitSecret(byte[]? stored, bool password)
    {
        var s = new byte[8];
        if (stored is { Length: 8 } && stored.Any(b => b != 0)) stored.CopyTo(s, 0);
        var want = password ? SecretBytesWithPassword : SecretBytes;
        if (s.All(b => b == 0))
            do RandomNumberGenerator.Fill(s.AsSpan(0, want)); while (s.Take(want).All(b => b == 0));
        else if (s[6] != 0 || s[7] != 0 || password && (s[4] != 0 || s[5] != 0))
            Array.Clear(s, want, s.Length - want);
        else if (!password && s[4] == 0 && s[5] == 0)
            do RandomNumberGenerator.Fill(s.AsSpan(4, 2)); while (s[4] == 0 && s[5] == 0);
        return s;
    }

    /// <summary>The host's secret for this password state, read from (and, when the game is not running, saved to) the
    /// profile's party_secret.txt. Saving what the engine would save keeps the code the same when the game starts.</summary>
    public static byte[] HostSecret(string userDir, bool password, bool save)
    {
        var path = Path.Combine(userDir, "party_secret.txt");
        string? stored = null;
        try { if (File.Exists(path)) stored = File.ReadAllText(path); }
        catch (IOException) { }
        var secret = FitSecret(ParseSecret(stored), password);
        var hex = Convert.ToHexString(secret).ToLowerInvariant();
        if (save && stored?.Trim().ToLowerInvariant() != hex)
        {
            try { Directory.CreateDirectory(userDir); File.WriteAllText(path, hex + "\n"); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
        return secret;
    }

    /// <summary>The code lines of the engine's party_code.txt ("internet=...", "lan=...") while hosting; "" for a missing one.</summary>
    public static (string Internet, string Lan) ReadFile(string userDir)
    {
        string internet = "", lan = "";
        try
        {
            foreach (var line in File.ReadAllLines(Path.Combine(userDir, "party_code.txt")))
            {
                if (line.StartsWith("internet=")) internet = line[9..].Trim();
                else if (line.StartsWith("lan=")) lan = line[4..].Trim();
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        return (internet, lan);
    }

    /// <summary>The public IPv4 for the Internet code: the Host public address when it is an IPv4 address or a name that
    /// resolves to one, else the address a STUN server sees (the same lookups the engine makes). Null when neither answers
    /// or STUN is off.</summary>
    public static async Task<string?> PublicAddressAsync(string overrideAddress, string stunServer, CancellationToken cancel = default)
    {
        overrideAddress = overrideAddress.Trim();
        if (overrideAddress.Length > 0)
        {
            var host = overrideAddress.Contains(':') ? overrideAddress[..overrideAddress.LastIndexOf(':')] : overrideAddress;
            if (await ResolveAsync(host) is { } resolved) return resolved;
        }
        stunServer = stunServer.Trim();
        if (stunServer is "off" or "0") return null;
        if (stunServer.Length == 0) stunServer = DefaultStun;
        var port = 19302;
        var name = stunServer;
        if (stunServer.LastIndexOf(':') is var colon and > 0 && int.TryParse(stunServer[(colon + 1)..], out var parsed))
        {
            name = stunServer[..colon];
            port = parsed;
        }
        if (port is < 1 or > 65535 || await ResolveAsync(name) is not { } server) return null;
        for (var attempt = 0; attempt < 3 && !cancel.IsCancellationRequested; attempt++)
        {
            var found = await StunOnceAsync(IPAddress.Parse(server), port, TimeSpan.FromSeconds(2));
            if (found != null) return found;
        }
        return null;
    }

    private static async Task<string?> ResolveAsync(string host)
    {
        if (IPAddress.TryParse(host, out var literal)) return literal.AddressFamily == AddressFamily.InterNetwork ? literal.ToString() : null;
        try
        {
            var all = await Dns.GetHostAddressesAsync(host);
            return all.FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork)?.ToString();
        }
        catch (SocketException) { return null; }
    }

    private static async Task<string?> StunOnceAsync(IPAddress server, int port, TimeSpan timeout)
    {
        try
        {
            using var udp = new UdpClient(AddressFamily.InterNetwork);
            var request = new byte[20];
            request[1] = 1;  // Binding request, no attributes
            request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;  // magic cookie
            RandomNumberGenerator.Fill(request.AsSpan(8, 12));
            await udp.SendAsync(request, new IPEndPoint(server, port));
            using var timer = new CancellationTokenSource(timeout);
            var reply = await udp.ReceiveAsync(timer.Token);
            return ParseBindingResponse(reply.Buffer, request.AsSpan(8, 12));
        }
        catch (Exception e) when (e is SocketException or OperationCanceledException or ObjectDisposedException) { return null; }
    }

    /// <summary>The IPv4 address in a STUN Binding success response to the request with this transaction id
    /// (XOR-MAPPED-ADDRESS, or the old MAPPED-ADDRESS). Null for anything else.</summary>
    public static string? ParseBindingResponse(ReadOnlySpan<byte> data, ReadOnlySpan<byte> transaction)
    {
        if (data.Length < 20 || data[0] != 0x01 || data[1] != 0x01 || !data.Slice(8, 12).SequenceEqual(transaction)) return null;
        var end = Math.Min(data.Length, 20 + (data[2] << 8 | data[3]));
        string? plain = null;
        for (var at = 20; at + 4 <= end;)
        {
            var type = data[at] << 8 | data[at + 1];
            var length = data[at + 2] << 8 | data[at + 3];
            var value = at + 4;
            if (value + length > end) break;
            if ((type == 0x0020 || type == 0x0001) && length >= 8 && data[value + 1] == 1)
            {
                var cookie = new byte[] { 0x21, 0x12, 0xA4, 0x42 };
                var ip = new byte[4];
                for (var i = 0; i < 4; i++) ip[i] = (byte)(data[value + 4 + i] ^ (type == 0x0020 ? cookie[i] : 0));
                var text = new IPAddress(ip).ToString();
                if (type == 0x0020) return text;
                plain = text;
            }
            at = value + (length + 3 & ~3);
        }
        return plain;
    }
}
