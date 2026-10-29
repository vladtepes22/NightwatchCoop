using System.IO;
using BloodborneLauncher.Core;

/// <summary>The launcher's party codes against the engine's: the expected codes come from party_code.cpp's make_party_code
/// (compiled alone, 8 cases), so a change in either encoder shows here.</summary>
internal static class PartyCodeTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    internal static void Run()
    {
        var six = new byte[] { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0, 0 };
        var four = new byte[] { 0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0 };
        var cases = new (bool Lan, bool Password, byte[] Ip, int Port, byte[] Secret, string Code)[]
        {
            (false, false, new byte[] { 203, 0, 113, 7 }, 9307, six, "BBP3-HJR0-E43G-28T5-CY4T-QXD"),
            (false, true, new byte[] { 203, 0, 113, 7 }, 9307, four, "BBP3-NJR0-E43X-XBDY-XZMY"),
            (false, false, new byte[] { 203, 0, 113, 7 }, 9400, six, "BBP3-1JR0-E43J-9E01-4D2P-F2DB-30"),
            (false, true, new byte[] { 203, 0, 113, 7 }, 9400, four, "BBP3-5JR0-E43J-9E6Y-NPZE-ZG6"),
            (true, false, new byte[] { 192, 168, 1, 22 }, 9307, six, "BBP3-J5G1-4D2P-F2DB-9D"),
            (true, true, new byte[] { 192, 168, 1, 22 }, 9307, four, "BBP3-P5PY-NPZE-ZP4"),
            (true, true, new byte[] { 26, 4, 5, 200 }, 9500, four, "BBP3-7J15-3KFA-VFQF-XW"),
            (false, false, new byte[] { 255, 255, 255, 255 }, 65535, six, "BBP3-1ZZZ-ZZZZ-ZZR1-4D2P-F2DB-ED"),
        };
        foreach (var c in cases)
            Check(PartyCodes.Encode(c.Lan, c.Password, c.Ip, c.Port, c.Secret) == c.Code,
                $"Party code differs from the engine's: {c.Code} (got {PartyCodes.Encode(c.Lan, c.Password, c.Ip, c.Port, c.Secret)})");

        // The secret: a missing one is made at the size the password state needs; a stored one is shortened with a password and
        // lengthened (new random bits) without; an unchanged one stays.
        var made = PartyCodes.FitSecret(null, false);
        Check(made.Length == 8 && made.Take(6).Any(b => b != 0) && made[6] == 0 && made[7] == 0, "New secret without a password is not 6 bytes");
        var madeWithPassword = PartyCodes.FitSecret(null, true);
        Check(madeWithPassword.Take(4).Any(b => b != 0) && madeWithPassword.Skip(4).All(b => b == 0), "New secret with a password is not 4 bytes");
        Check(PartyCodes.FitSecret(six, false).SequenceEqual(six), "A 6-byte secret without a password changed");
        Check(PartyCodes.FitSecret(four, true).SequenceEqual(four), "A 4-byte secret with a password changed");
        Check(PartyCodes.FitSecret(six, true).SequenceEqual(new byte[] { 0x01, 0x23, 0x45, 0x67, 0, 0, 0, 0 }), "A 6-byte secret was not shortened with a password");
        var lengthened = PartyCodes.FitSecret(four, false);
        Check(lengthened.Take(4).SequenceEqual(four.Take(4)) && (lengthened[4] != 0 || lengthened[5] != 0) && lengthened[6] == 0, "A 4-byte secret was not lengthened without a password");
        Check(PartyCodes.ParseSecret("0123456789abcdef") != null && PartyCodes.ParseSecret("0000000000000000") == null &&
              PartyCodes.ParseSecret("xyz") == null && PartyCodes.ParseSecret("0123456789abcdeg") == null, "Secret file parsing changed");

        // party_secret.txt: made and saved once (as the engine would), kept across calls, and not written when the game runs.
        var folder = Path.Combine(Path.GetTempPath(), "nw-partycodes-" + Guid.NewGuid().ToString("N"));
        try
        {
            var file = Path.Combine(folder, "party_secret.txt");
            var unsaved = PartyCodes.HostSecret(folder, false, save: false);
            Check(!File.Exists(file) && unsaved.Length == 8, "A running game's secret file was written");
            var first = PartyCodes.HostSecret(folder, false, save: true);
            Check(File.Exists(file) && PartyCodes.ParseSecret(File.ReadAllText(file))!.SequenceEqual(first), "The secret was not saved in the engine's format");
            Check(PartyCodes.HostSecret(folder, false, save: true).SequenceEqual(first), "The saved secret changed on the next read");
            var shortened = PartyCodes.HostSecret(folder, true, save: true);
            Check(shortened.Take(4).SequenceEqual(first.Take(4)) && shortened[4] == 0, "Setting a password did not shorten the saved secret");
            File.WriteAllText(Path.Combine(folder, "party_code.txt"), "internet=BBP3-AAAA\nlan=BBP3-BBBB\n");
            Check(PartyCodes.ReadFile(folder) is ("BBP3-AAAA", "BBP3-BBBB"), "The engine's code file was not read");
        }
        finally { if (Directory.Exists(folder)) Directory.Delete(folder, true); }

        // STUN: the XOR-mapped address of a Binding response (RFC 5389), a plain MAPPED-ADDRESS, and a response to another request.
        var transaction = Enumerable.Range(1, 12).Select(i => (byte)i).ToArray();
        byte[] Response(byte type, byte[] address, bool xor)
        {
            var cookie = new byte[] { 0x21, 0x12, 0xA4, 0x42 };
            var body = new byte[] { 0, type, 0, 8, 0, 1, 0x12, 0x34 }.Concat(address.Select((b, i) => (byte)(xor ? b ^ cookie[i] : b))).ToArray();
            return new byte[] { 1, 1, 0, (byte)body.Length, 0x21, 0x12, 0xA4, 0x42 }.Concat(transaction).Concat(body).ToArray();
        }
        var ip = new byte[] { 198, 51, 100, 23 };
        Check(PartyCodes.ParseBindingResponse(Response(0x20, ip, true), transaction) == "198.51.100.23", "XOR-MAPPED-ADDRESS not read");
        Check(PartyCodes.ParseBindingResponse(Response(0x01, ip, false), transaction) == "198.51.100.23", "MAPPED-ADDRESS not read");
        Check(PartyCodes.ParseBindingResponse(Response(0x20, ip, true), new byte[12]) == null, "A response to another request was accepted");
        Check(PartyCodes.ParseBindingResponse(new byte[10], transaction) == null, "A short packet was accepted");
    }
}
