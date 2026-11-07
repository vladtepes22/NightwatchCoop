using System.IO;
using System.Text;
using System.Text.Json.Nodes;
using BloodborneLauncher.Core;

/// <summary>The host's mods label as visy PR 34 (game 0.7.9) carries it: the LAN 'M' datagram, the "[host mods: L]" refusal
/// prefix and party_host_mods.txt; each read only when present. Then the automatic build-and-join after a refusal over mods
/// that names a randomized world, with fakes (no game, no generator), and the generator's progress lines.</summary>
internal static class WorldRejoinTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException("World rejoin: " + message);
    }

    private static byte[] Announce(uint id, string name = "Hunter")
    {
        var bytes = new List<byte> { 0xfa, (byte)'A', 1, 2, (byte)(id >> 24), (byte)(id >> 16), (byte)(id >> 8), (byte)id, 0x24, 0x4b, 0, 42, 1, 3 };
        foreach (var s in new[] { "nightwatch-coop-0.2.8-beta.1-x", name, "" }) { bytes.Add((byte)s.Length); bytes.AddRange(Encoding.ASCII.GetBytes(s)); }
        return bytes.ToArray();
    }

    // party_discovery.cpp encode_mods (PR 34): fa 'M' 1, id u32 big endian, u8 length, the label.
    private static byte[] Mods(uint id, string label, int version = 1, int? length = null)
    {
        var bytes = new List<byte> { 0xfa, (byte)'M', (byte)version, (byte)(id >> 24), (byte)(id >> 16), (byte)(id >> 8), (byte)id, (byte)(length ?? label.Length) };
        bytes.AddRange(Encoding.Latin1.GetBytes(label));
        return bytes.ToArray();
    }

    internal static void Run()
    {
        // ---- the generator's progress lines (Randomizer.ParseProgress): the bar only moves forward over the steps
        var last = -1.0;
        foreach (var line in new[] { "progress read 0 1", "progress read 1 1", "progress plan 0 1", "progress maps 1 16", "progress maps 16 16",
                                     "progress effects 0 1", "progress items 0 1", "progress text 0 1", "progress write 0 1", "progress write 1 1" })
        {
            Check(Randomizer.ParseProgress(line) is { } p && p.Fraction >= last, "progress goes back at: " + line);
            last = Randomizer.ParseProgress(line)!.Value.Fraction;
        }
        Check(last == 1.0 && Randomizer.ParseProgress("progress maps 20 16") is { } past && Math.Abs(past.Fraction - 0.70) < 1e-9, "a step's count past its total stays at the step's end");
        Check(Randomizer.ParseProgress("progress maps x 16") == null && Randomizer.ParseProgress("  progress read 0 1\r") is { Step: "read" } &&
              Randomizer.ParseProgress("progress read 0 1 extra") == null, "progress lines are read strictly (a CR from the pipe is fine)");
        Check(Randomizer.StepText("maps").Length > 0 && Randomizer.StepText("nope") == Randomizer.StepText("other"), "every step has a text");

        // ---- the LAN 'M' datagram
        const string label = "nocturne@1.0.1";
        Check(PartyDiscovery.ParseMods(Mods(0xdeadbeef, label)) is { Id: 0xdeadbeef, Label: label }, "a mods datagram is read");
        Check(PartyDiscovery.ParseMods(Mods(1, new string('a', 255))) is { Label.Length: 255 } && Mods(1, new string('a', 255)).Length == 263,
            "the longest label (263 bytes) is read");
        var badMods = new Dictionary<string, byte[]>
        {
            ["empty label"] = Mods(1, ""), ["version 2"] = Mods(1, label, version: 2), ["short length"] = Mods(1, label, length: label.Length - 1),
            ["long length"] = Mods(1, label, length: label.Length + 1), ["upper case"] = Mods(1, "Nocturne@1.0"), ["space"] = Mods(1, "a b"),
            ["slash"] = Mods(1, "x/../y"), ["high byte"] = Mods(1, "noctérne"), ["magic"] = new byte[] { 0xfb }.Concat(Mods(1, label).Skip(1)).ToArray(),
            ["trailing"] = Mods(1, label).Concat(new byte[] { 0 }).ToArray(), ["header only"] = Mods(1, label)[..8],
            ["an announcement"] = Announce(1),
        };
        foreach (var (name, packet) in badMods) Check(PartyDiscovery.ParseMods(packet) == null, "a bad mods datagram was read: " + name);
        Check(PartyDiscovery.Parse(Mods(1, label)) == null, "the announcement parser drops a mods datagram (as an older launcher does)");

        // The book: a label goes with the announcement of the same id from the same address; none heard: "" (unknown).
        var book = new PartyDiscovery.Book();
        Check(book.Feed(Announce(7, "Alpha"), "10.0.0.2", 1000) && book.Feed(Mods(7, label), "10.0.0.2", 1001), "an announcement and its label go in");
        Check(book.Feed(Announce(8, "Beta"), "10.0.0.3", 1000) && book.Feed(Mods(8, "mods:3f9a12c0"), "10.0.0.9", 1001), "another source's label goes in");
        book.Feed(Announce(9, "Gamma"), "10.0.0.4", 1000);
        var heard = book.Snapshot(1500).ToDictionary(p => p.Info.Name, p => p.Mods);
        Check(heard["Alpha"] == label && heard["Beta"] == "" && heard["Gamma"] == "", "labels by (address, id): " + string.Join(", ", heard));
        Check(!book.Feed(Mods(7, "Bad Label"), "10.0.0.2", 1600) && book.Snapshot(1600).Single(p => p.Info.Name == "Alpha").Mods == label,
            "a bad label changes nothing");
        book.Feed(Announce(7, "Alpha"), "10.0.0.2", 7000);
        Check(book.Snapshot(7100).Single(p => p.Info.Name == "Alpha").Mods == "", "a label not heard again expires with the entry's time to live");
        var flood = new PartyDiscovery.Book();
        for (var i = 0; i < PartyDiscovery.MaxPerSecond; i++) flood.Feed(Mods(5, label), "10.0.0.5", 2000);
        Check(!flood.Feed(Announce(5), "10.0.0.5", 2000), "mods datagrams count in the per-source rate limit");
        book.Clear();
        Check(book.Snapshot(7100).Count == 0, "Clear forgets labels too");

        // ---- the refusal: "[host mods: L] " (an older guest game shows it in the reason), "(the host's mods: L)" (a newer one)
        var world = "nocturne@1.0.1+rando2:r2-0w000-44jm2-00mp5+w:f0397280+g:8802bfef";
        Check(WorldRejoin.LabelFromText($"host refused: [host mods: {world}] version mismatch with the host: gameplay mods differ (host only: X; yours only: -)") == world,
            "an older guest's text (options prefix already taken off) names the label");
        Check(WorldRejoin.LabelFromText($"[host options: max=4 lamps=0 headstones=1 talk=1 droproll=1] [host mods: {world}] version mismatch with the host: gameplay mods differ (x)") == world,
            "the label after the options prefix");
        Check(WorldRejoin.LabelFromText($"host refused: version mismatch with the host: gameplay mods differ (x) (the host's mods: {label})") == label,
            "a newer guest's error text names the label");
        foreach (var forged in new[] { "[host mods: Nocturne] x", "[host mods: a b] x", "[host mods: ] x", "[host mods: x/../y] z", "version mismatch with the host: gameplay mods differ (x)", "" })
            Check(WorldRejoin.LabelFromText(forged) == null, "no label read from: " + forged);
        Check(WorldRejoin.LabelFromText("[host mods: " + new string('a', 256) + "] x") == null, "a label over 255 is no label");

        var tmp = Path.Combine(Path.GetTempPath(), "nw-rejoin-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(tmp);
            var file = Path.Combine(tmp, WorldRejoin.HostModsFile);
            var start = DateTime.UtcNow;
            Check(WorldRejoin.ReadHostModsFile(tmp, start) == null, "no file: no label");
            File.WriteAllText(file, world + "\n");
            Check(WorldRejoin.ReadHostModsFile(tmp, start) == world, "party_host_mods.txt is read");
            File.SetLastWriteTimeUtc(file, start.AddMinutes(-10));
            Check(WorldRejoin.ReadHostModsFile(tmp, start) == null, "a file from before this game's start is old (it stays after a join)");
            File.WriteAllText(file, "Not A Label\n");
            Check(WorldRejoin.ReadHostModsFile(tmp, start.AddMinutes(-1)) == null, "an invalid label in the file is no label");

            SeamlessSession.Status Status(string role, string state, string detail = "", string alert = "", string @event = "") =>
                new(role, state, detail, "", @event, alert, "", Array.Empty<SeamlessSession.Member>());
            File.WriteAllText(file, world + "\n");
            Check(WorldRejoin.RefusedHostLabel(Status("guest", "rejected"), tmp, start.AddMinutes(-1)) == world, "a refused guest reads the file");
            Check(WorldRejoin.RefusedHostLabel(Status("guest", "connected", "In the party"), tmp, start.AddMinutes(-1)) == null, "a joined guest is not refused (the file stays)");
            Check(WorldRejoin.RefusedHostLabel(Status("host", "rejected"), tmp, start.AddMinutes(-1)) == null, "a host is never a refused guest");
            Check(WorldRejoin.RefusedHostLabel(null, tmp, start) == null, "no status: nothing");
            File.Delete(file);
            Check(WorldRejoin.RefusedHostLabel(Status("guest", "error", alert: $"The host refused you: [host mods: {world}] gameplay mods differ"), tmp, start) == world,
                "without the file the status text is read (an older guest game)");
            Check(WorldRejoin.RefusedHostLabel(Status("guest", "rejected", "host refused: gameplay mods differ (host only: Randomizer R2-X 00000000)"), tmp, start) == null,
                "a host before 0.7.9 sends no label: nothing happens");
        }
        finally
        {
            try { Directory.Delete(tmp, true); } catch (IOException) { }
        }

        // ---- the automatic answer (fakes): a refusal naming a world builds it, then joins again; once per label
        var vectors = JsonNode.Parse(File.ReadAllText(WorldJoinTests.RepoFile(Path.Combine("tools", "randomizer", "tests", "r2_vectors.json"))))!;
        var code = vectors["vectors"]!.AsArray().Select(v => v!["error"] == null ? v["canonical"]!.GetValue<string>() : null).First(c => c != null)!;
        var hostLabel = $"rando{Randomizer.GeneratorMajor}:{code.ToLowerInvariant()}+w:1b8e536a";
        var plain = new LocalMods("standard", null, 0, CanBuildWorlds: true);
        var advice = CuratedMods.JoinAdvice(hostLabel, plain, null);
        Check(advice.Kind == JoinKind.BuildWorld, "the fake host's world is one to build: " + advice);
        WorldRejoin.Reset();
        var steps = new List<string>();
        var fractions = new List<double>();
        bool Run(string label2, JoinChoice choice) => WorldRejoin.RunAsync(label2, choice,
            () => { steps.Add("stop"); return Task.CompletedTask; },
            (c, progress) => { steps.Add("build " + c.Code); progress(0.5, "half"); progress(1, "done"); return Task.CompletedTask; },
            () => { steps.Add("join"); return Task.CompletedTask; },
            (f, _) => fractions.Add(f)).GetAwaiter().GetResult();
        Check(Run(hostLabel, advice), "a refusal naming a world joins again");
        Check(steps.SequenceEqual(new[] { "stop", "build " + code, "join" }), "stop, build, join, in that order: " + string.Join(", ", steps));
        Check(fractions.SequenceEqual(new[] { 0.0, 0.5, 1.0, 1.0 }), "the bar follows the build: " + string.Join(", ", fractions));
        steps.Clear();
        Check(!Run(hostLabel, advice) && steps.Count == 0, "a second refusal for the same world is left to the player (no loop)");
        Check(!Run("nocturne@1.0.1", CuratedMods.JoinAdvice("nocturne@1.0.1", plain, new[] { "1.0.1" })) && steps.Count == 0, "a Nocturne host builds nothing");
        Check(!Run("mods:3f9a12c0", CuratedMods.JoinAdvice("mods:3f9a12c0", plain, null)) && steps.Count == 0, "other mods build nothing");
        var playing = new LocalMods("world", null, 0, World: $"Randomizer {code} 1b8e536a", CanBuildWorlds: true, WorldGenerator: Randomizer.GeneratorMajor);
        WorldRejoin.Reset();
        Check(!Run(hostLabel, CuratedMods.JoinAdvice(hostLabel, playing, null)) && steps.Count == 0, "the world already plays here: another cause, nothing built");
        Check(!Run(hostLabel, CuratedMods.JoinAdvice(hostLabel, plain with { CanBuildWorlds = false }, null)) && steps.Count == 0, "a package without the generator builds nothing");
        var failed = false;
        try
        {
            WorldRejoin.RunAsync(hostLabel, advice, () => Task.CompletedTask, (_, _) => throw new InvalidDataException("test: build failed"),
                () => { steps.Add("join"); return Task.CompletedTask; }, (_, _) => { }).GetAwaiter().GetResult();
        }
        catch (InvalidDataException) { failed = true; }
        Check(failed && steps.Count == 0, "a failed build joins nothing and tells why");
        WorldRejoin.Reset();

        // The join again goes to the same host: a join start records its target, an invasion does not (a scratch data folder:
        // a join start reads and removes party_adopt.txt in the user folder).
        var dataBefore = Environment.GetEnvironmentVariable("BB_DATA_DIR");
        var data = Path.Combine(Path.GetTempPath(), "nw-rejoin-data-" + Guid.NewGuid().ToString("N"));
        Environment.SetEnvironmentVariable("BB_DATA_DIR", data);
        try { JoinTargets(); }
        finally
        {
            Environment.SetEnvironmentVariable("BB_DATA_DIR", dataBefore);
            try { if (Directory.Exists(data)) Directory.Delete(data, true); } catch (IOException) { }
        }

        // The BBP3 party code has no spare bit (visy PR 34: all four flag bits are used, the rest is the CRC), so a code says
        // nothing about the host's mods; the refusal above is the path. The codes stay as they were.
        Check(PartyCodes.Encode(false, false, new byte[] { 1, 2, 3, 4 }, 9307, new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 }).StartsWith("BBP3-"), "codes unchanged");        Console.WriteLine("PASS: world rejoin: progress lines, LAN mods label datagram and book, refusal label (text and party_host_mods.txt), build then join again on a fake refusal, join target");
    }

    private static void JoinTargets()
    {
        var s = new LauncherSettings();
        s.Set("party_mode", "join");
        s.Set("party_code", "192.168.1.20:9307");
        SeamlessSettings.Apply(s, new Dictionary<string, string>());
        Check(WorldRejoin.LastJoin is { Code: "192.168.1.20:9307" }, "a join start records its host: " + WorldRejoin.LastJoin);
        SeamlessSettings.PendingJoin = ("10.8.0.7:9307", "pw");
        SeamlessSettings.PendingHostOptions = new() { ["BB_PARTY_MAX"] = "4" };
        try
        {
            SeamlessSettings.Apply(s, new Dictionary<string, string>());
            Check(WorldRejoin.LastJoin is { Code: "10.8.0.7:9307", Password: "pw", HostOptions: { } o } && o["BB_PARTY_MAX"] == "4", "a lobby join records its host and options");
            SeamlessSettings.PendingInvade = true;
            SeamlessSettings.Apply(s, new Dictionary<string, string>());
            Check(WorldRejoin.LastJoin == null, "an invasion is never joined again by itself");
        }
        finally { SeamlessSettings.PendingJoin = null; SeamlessSettings.PendingHostOptions = null; SeamlessSettings.PendingInvade = false; }
        s.Set("party_mode", "host");
        SeamlessSettings.Apply(s, new Dictionary<string, string>());
        Check(WorldRejoin.LastJoin == null, "a host start forgets the join");

        // the guest's copy built after a refusal is named after the host (WorldSaves.NameForGuest), "Host's world" without one
        var members = new[] { new SeamlessSession.Member("Me", true, true, true, 0, ""), new SeamlessSession.Member("Anna", true, true, false, 20, "") };
        var refused = new SeamlessSession.Status("guest", "refused", "", "", "", "", "", members);
        Check(WorldRejoin.HostName(refused) == "Anna", "the rejoin names the copy after the host's lobby name");
        Check(WorldRejoin.HostName(refused with { members = new[] { members[0] } }) == "" && WorldRejoin.HostName(null) == "",
            "no host listed: no name");
        Check(WorldSaves.GuestName(WorldRejoin.HostName(null)) == WorldSaves.GuestName(""), "then the copy is the default guest name");
    }
}
