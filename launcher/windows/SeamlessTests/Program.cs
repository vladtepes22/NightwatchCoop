using BloodborneLauncher.Core;
using System.IO;

// --render-ui <folder> [language]: the pages as they lay out, in a language of Localization.Languages
// (no language: English, as the renderer has always drawn them).
if ((args.Length == 2 || args.Length == 3) && args[0] == "--render-ui")
{
    if (args.Length == 3) Localization.Initialize(args[2]);
    UiRender.Check(args[1]);
    return;
}
// --map-prep <package> <game> <user folder>: the launcher's map preparation (MapPrep) end to end
// with a package's generator and baker, as on a tester's first start; no window, no game.
if (args.Length == 4 && args[0] == "--map-prep")
{
    Paths.PortDir = args[1];
    var prep = new LauncherSettings();
    prep.Set("game_dir", args[2]);
    prep.Set("user_dir", args[3]);
    if (!MapPrep.Supported) throw new InvalidOperationException("No map generator in " + args[1]);
    var mapEnv = new Dictionary<string, string>();
    MapPrep.AddEnvironment(prep, mapEnv);
    // Off by default: no map for the game, nothing made.
    if (mapEnv.Count != 0) throw new InvalidOperationException("The map is on without the player turning it on");
    prep.Set("map_enabled", true);
    MapPrep.AddEnvironment(prep, mapEnv);
    Console.WriteLine($"BB_MAP_DIR={mapEnv["BB_MAP_DIR"]} (exists: {Directory.Exists(mapEnv["BB_MAP_DIR"])})");
    var last = "";
    MapPrep.Changed += () =>
    {
        var line = $"{MapPrep.State} {MapPrep.Percent}% {MapPrep.Step}";
        if (line != last) Console.WriteLine($"{DateTime.Now:HH:mm:ss} {line}");
        last = line;
    };
    var watch = System.Diagnostics.Stopwatch.StartNew();
    MapPrep.Start(prep);
    while (MapPrep.State == MapPrep.Phase.Preparing) Thread.Sleep(200);
    Console.WriteLine($"{MapPrep.State} after {watch.Elapsed.TotalSeconds:0} s {MapPrep.Error}");
    Console.WriteLine($"ready: {MapPrep.IsReady(prep)}, index: {File.Exists(Path.Combine(mapEnv["BB_MAP_DIR"], "atlas2-index.json"))}");
    MapPrep.Start(prep);  // already made: nothing runs again
    Console.WriteLine($"second start: {MapPrep.State}");
    Environment.Exit(MapPrep.IsReady(prep) ? 0 : 1);
}

static void Check(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

var settings = new LauncherSettings();
settings.Set("party_mode", "host");
settings.Set("party_name", "Host_Test");
settings.Set("party_port", "47890");
settings.Set("party_max", "4");
settings.PartyPassword = "test secret";
var env = new Dictionary<string, string> { ["BB_UPSCALER"] = "dlss", ["BB_HDR"] = "1", ["BB_DRAW_AHEAD"] = "2", ["BB_ONLINE"] = "1", ["BB_PARTY_RESTARTED"] = "1", ["BB_PARTY_AUTOCONTINUE"] = "1" };
SeamlessSettings.Apply(settings, env);
Check(SeamlessSettings.Validate(settings) == null, "Valid host rejected");
Check(env["BB_PARTY"] == "host" && env["BB_PARTY_MAX"] == "4", "Host setup missing");
Check(env["BB_PARTY_PASSWORD"] == "test secret", "Password did not reach runtime");
Check(env["BB_ONLINE"] == "0" && env["BB_PARTY_SAVE"] == "separate", "Network ownership or save isolation missing");
Check(env["BB_UPSCALER"] == "dlss" && env["BB_HDR"] == "1" && env["BB_DRAW_AHEAD"] == "2", "Base renderer options changed");
// The game's co-op switches come from bbport.ini (where the in-game menu changes them): a launcher
// variable would undo an in-game change at the next start. Only the launcher's own switches go out.
Check(!env.ContainsKey("BB_PARTY_ITEMS") && !env.ContainsKey("BB_PARTY_PROGRESS") && !env.ContainsKey("BB_PARTY_GUEST_INSIGHT") && !env.ContainsKey("BB_PARTY_RENDEZVOUS")
      && env["BB_PARTY_RESTART"] == "1", "The game's co-op switches were passed as variables, or the restart switch is missing");
var moveSettings = new LauncherSettings();
moveSettings.Set("party_items", false);
moveSettings.Set("party_story", true);
moveSettings.Set("party_guest_insight", "full");
moveSettings.Set("party_rendezvous", true);
var moveIni = new IniFile();
moveIni.Set("party_story", "0");  // changed in the game already: kept
Check(SeamlessSettings.MoveToIni(moveSettings, moveIni) && moveIni.Raw("party_items") == "0" && moveIni.Raw("party_story") == "0"
      && moveIni.Raw("party_guest_insight") == "full" && moveIni.Raw("party_rendezvous") == "1" && moveIni.Raw("party_progress") == null,
      "An older launcher's co-op switches did not move to bbport.ini as they were");
Check(!SeamlessSettings.MoveToIni(moveSettings, moveIni), "The co-op switches moved to bbport.ini a second time");
Check(env["BB_PARTY_AUTOCONTINUE"] == "0" && env["BB_PARTY_RESTARTED"] == "0", "Manual launch inherited automated Continue");
// The map button shares Options / Start on purpose (held: the map): moving either never swaps the
// other, and Restore puts the map back on Start.
var padIni = new IniFile();
Check(PadMap.First(padIni, "map") == "start" && PadMap.MapButtonShared(padIni), "The map is not on a held Options / Start by default");
PadMap.Assign(padIni, "map", "paddle1");
Check(PadMap.First(padIni, "options") == "start" && !PadMap.MapButtonShared(padIni), "Moving the map changed Options, or a button of its own reads as shared");
PadMap.Assign(padIni, "map", "back");
Check(PadMap.Get(padIni, "touchpad").Contains("back") && PadMap.MapButtonShared(padIni), "The map took Back from the touchpad");
PadMap.Assign(padIni, "options", "y");
Check(PadMap.First(padIni, "map") == "back" && PadMap.First(padIni, "triangle") == "start", "Moving Options moved the map");
PadMap.Reset(padIni, "map");
Check(padIni.Raw("pad.map") == null && PadMap.First(padIni, "map") == "start", "Restoring the map button left it off Start");
Check(InputMap.Controls.Single(c => c.Name == "map").Default == "M", "The map's key is not M by default");
var commands = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "seamless-stop-" + Guid.NewGuid());
System.IO.Directory.CreateDirectory(commands);
try
{
    SeamlessSession.PostCommand(commands, "quit");
    using var command = System.Text.Json.JsonDocument.Parse(System.IO.File.ReadAllText(System.IO.Path.Combine(commands, "party_command.json")));
    Check(command.RootElement.GetProperty("action").GetString() == "quit", "Graceful stop command missing");
    Check(!System.IO.File.Exists(System.IO.Path.Combine(commands, "party_command.json.tmp")), "Stop command was not committed");
}
finally { System.IO.Directory.Delete(commands, true); }
settings.Set("party_restart", false);
SeamlessSettings.Apply(settings, env);
Check(env["BB_PARTY_RESTART"] == "0", "Disabled launcher switch ignored");
settings.Set("party_mode", "join");
Check(SeamlessSettings.Validate(settings) != null, "Empty join target accepted");
settings.Set("party_code", "127.0.0.1:47890");
Check(SeamlessSettings.Validate(settings) == null, "Valid join target rejected");
settings.Set("party_port", "65536");
Check(SeamlessSettings.Validate(settings) != null, "Invalid port accepted");
settings.Set("party_port", "47890");
settings.Set("party_name", "bad name");
Check(SeamlessSettings.Validate(settings) != null, "Invalid name accepted");
settings.Set("party_mode", "");
SeamlessSettings.Apply(settings, env);
Check(env["BB_PARTY"] == "off", "Single-player mode still enables party code");
// Updater: a package file it cannot replace stops the update before the launcher and
// package.json (the installed engine ID) are touched, so a half-copied engine is never called current.
// Session logs: each start archives the last one to logs\sessions (newest 10 kept); a crash's log
// no longer disappears after two more starts.
var logsDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "seamless-logs-" + Guid.NewGuid());
System.IO.Directory.CreateDirectory(logsDir);
var lastLog = System.IO.Path.Combine(logsDir, "last_session.log");
var firstEnd = new DateTime(2026, 10, 23, 2, 0, 0);
for (var i = 0; i < 12; i++)
{
    System.IO.File.WriteAllText(lastLog, "session " + i);
    System.IO.File.SetLastWriteTime(lastLog, firstEnd.AddMinutes(i));
    GameRunner.ArchiveSessionLog(lastLog, GameRunner.KeptSessionLogs);
}
System.IO.File.WriteAllText(lastLog, "same second");
System.IO.File.SetLastWriteTime(lastLog, firstEnd.AddMinutes(11));
GameRunner.ArchiveSessionLog(lastLog, 20);
var archived = System.IO.Directory.GetFiles(System.IO.Path.Combine(logsDir, "sessions")).Select(System.IO.Path.GetFileName).OrderBy(n => n, StringComparer.Ordinal).ToArray();
Check(archived.Length == 11 && archived[0] == "session-20261023-020200.log", "Session log archive kept the wrong logs: " + string.Join(", ", archived));
Check(archived.Contains("session-20261023-021100-2.log") && System.IO.File.ReadAllText(System.IO.Path.Combine(logsDir, "sessions", "session-20261023-021100.log")) == "session 11",
    "Two sessions ending in the same second overwrote each other");
System.IO.Directory.Delete(logsDir, true);
var copyPackage =typeof(Updater).GetMethod("CopyPackage", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
var update = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "seamless-update-" + Guid.NewGuid());
var newPackage = System.IO.Path.Combine(update, "new");
var installed = System.IO.Path.Combine(update, "installed");
foreach (var (folder, text) in new[] { (newPackage, "new"), (installed, "old") })
{
    System.IO.Directory.CreateDirectory(System.IO.Path.Combine(folder, "engine", "out"));
    foreach (var name in new[] { "Nightwatch Co-op.exe", "package.json", @"engine\out\libiconv-2.dll", @"engine\out\bb-probe.exe" })
        System.IO.File.WriteAllText(System.IO.Path.Combine(folder, name), text);
}
var installedExe = System.IO.Path.Combine(installed, "Nightwatch Co-op.exe");
string Read(string name) => System.IO.File.ReadAllText(System.IO.Path.Combine(installed, name));
try
{
    using (new System.IO.FileStream(System.IO.Path.Combine(installed, @"engine\out\libiconv-2.dll"), System.IO.FileMode.Open, System.IO.FileAccess.Read, System.IO.FileShare.Read))
    {
        var failed = false;
        try { copyPackage.Invoke(null, new object[] { newPackage, installed, installedExe, new List<string>() }); }
        catch (System.Reflection.TargetInvocationException) { failed = true; }
        Check(failed, "A locked engine file did not stop the update");
        Check(Read("Nightwatch Co-op.exe") == "old" && Read("package.json") == "old", "A failed update replaced the launcher or package.json");
    }
    copyPackage.Invoke(null, new object[] { newPackage, installed, installedExe, new List<string>() });
    Check(Read(@"engine\out\libiconv-2.dll") == "new" && Read("package.json") == "new" && Read("Nightwatch Co-op.exe") == "new", "A clean update did not replace everything");
}
finally { System.IO.Directory.Delete(update, true); }

// SDL3.dll (the Controls tab's) keeps out\libiconv-2.dll open while loaded: after SDL_Quit and
// FreeLibrary, as SdlPad.Unload does, the file can be replaced again. Needs MSYS2's copies.
var msysSdl = @"C:\msys64\clang64\bin";
if (System.IO.File.Exists(System.IO.Path.Combine(msysSdl, "SDL3.dll")) && System.IO.File.Exists(System.IO.Path.Combine(msysSdl, "libiconv-2.dll")))
{
    var sdlDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "seamless-sdl-" + Guid.NewGuid());
    System.IO.Directory.CreateDirectory(sdlDir);
    foreach (var name in new[] { "SDL3.dll", "libiconv-2.dll" })
        System.IO.File.Copy(System.IO.Path.Combine(msysSdl, name), System.IO.Path.Combine(sdlDir, name));
    var iconv = System.IO.Path.Combine(sdlDir, "libiconv-2.dll");
    var lib = System.Runtime.InteropServices.NativeLibrary.Load(System.IO.Path.Combine(sdlDir, "SDL3.dll"));
    var init = System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer<SdlInit>(System.Runtime.InteropServices.NativeLibrary.GetExport(lib, "SDL_InitSubSystem"));
    var quit = System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer<SdlQuit>(System.Runtime.InteropServices.NativeLibrary.GetExport(lib, "SDL_Quit"));
    init(0x2000);
    var locked = false;
    try { System.IO.File.Copy(System.IO.Path.Combine(msysSdl, "libiconv-2.dll"), iconv, true); } catch (System.IO.IOException) { locked = true; } catch (UnauthorizedAccessException) { locked = true; }
    Check(locked, "Loaded SDL3.dll did not lock libiconv-2.dll (the test no longer shows the update failure)");
    quit();
    System.Runtime.InteropServices.NativeLibrary.Free(lib);
    System.IO.File.Copy(System.IO.Path.Combine(msysSdl, "libiconv-2.dll"), iconv, true);
    System.IO.Directory.Delete(sdlDir, true);
}

// The map's input with the 1.09 update beside a 1.0 game: the update's eboot and sce_sys, the game's
// dvdroot through a junction, and the game folder left as it was. A path with a space on purpose.
{
    var maps = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "seamless map " + Guid.NewGuid());
    var game = System.IO.Path.Combine(maps, "CUSA03173");
    var upd = System.IO.Path.Combine(maps, "CUSA03173-patch");
    var root = System.IO.Path.Combine(maps, "root");
    System.IO.Directory.CreateDirectory(System.IO.Path.Combine(game, "dvdroot_ps4", "event"));
    System.IO.Directory.CreateDirectory(System.IO.Path.Combine(game, "sce_sys"));
    System.IO.Directory.CreateDirectory(System.IO.Path.Combine(upd, "sce_sys"));
    System.IO.Directory.CreateDirectory(root);
    System.IO.File.WriteAllText(System.IO.Path.Combine(game, "eboot.bin"), "1.00");
    System.IO.File.WriteAllText(System.IO.Path.Combine(game, "sce_sys", "param.sfo"), "1.00");
    System.IO.File.WriteAllText(System.IO.Path.Combine(game, "dvdroot_ps4", "event", "m.dcx"), "map");
    System.IO.File.WriteAllText(System.IO.Path.Combine(upd, "eboot.bin"), "1.09");
    System.IO.File.WriteAllText(System.IO.Path.Combine(upd, "sce_sys", "param.sfo"), "1.09");
    var input = MapPrep.PrepareInput(game, root);
    Check(input != game, "Map did not use the update beside the 1.0 game");
    Check(System.IO.File.ReadAllText(System.IO.Path.Combine(input, "eboot.bin")) == "1.09", "Map input has the 1.0 eboot");
    Check(System.IO.File.ReadAllText(System.IO.Path.Combine(input, "sce_sys", "param.sfo")) == "1.09", "Map input has the 1.0 param.sfo");
    Check(System.IO.File.Exists(System.IO.Path.Combine(input, "dvdroot_ps4", "event", "m.dcx")), "Map input lost the game's dvdroot");
    MapPrep.RemoveInput(input);
    Check(!System.IO.Directory.Exists(input), "Map input was not removed");
    Check(System.IO.File.Exists(System.IO.Path.Combine(game, "dvdroot_ps4", "event", "m.dcx")), "Removing the map input deleted the game's dvdroot");
    Check(System.IO.File.ReadAllText(System.IO.Path.Combine(game, "eboot.bin")) == "1.00", "The game folder's eboot was changed");
    MapPrep.RemoveInput(game);
    Check(System.IO.File.Exists(System.IO.Path.Combine(game, "dvdroot_ps4", "event", "m.dcx")), "Removing a map input deleted a game folder");
    System.IO.Directory.Delete(maps, true);
    var plain = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "seamless map plain " + Guid.NewGuid());
    System.IO.Directory.CreateDirectory(plain);
    Check(MapPrep.PrepareInput(plain, root) == plain, "Without an update the map reads the game folder itself");
    System.IO.Directory.Delete(plain, true);
}

// Start-up rules (0.7.5): the host refuses a guest whose values differ from its own, so the launcher names
// what is not the default and never treats an unset or unknown value as a difference.
{
    var stored = new Dictionary<string, string?>();
    string? ReadRule(string key) => stored.TryGetValue(key, out var v) ? v : null;
    Check(SeamlessSettings.StartupDifferences(ReadRule).Count == 0, "Unset start-up rules differ from the defaults");
    stored["party_guest_lamps"] = "1"; stored["party_guest_insight"] = "parity"; stored["party_start"] = "prologue_solo";
    stored["party_full_rewards"] = "bogus";
    Check(SeamlessSettings.StartupDifferences(ReadRule).Count == 0, "Default and unknown values count as a difference");
    stored["party_guest_lamps"] = "0"; stored["party_guest_insight"] = "full"; stored["party_start"] = "immediate";
    var differs = SeamlessSettings.StartupDifferences(ReadRule);
    Check(differs.SequenceEqual(new[] { "party_guest_lamps", "party_guest_insight", "party_start" }), "Differing start-up rules not named: " + string.Join(",", differs));
    stored["party_guest_insight"] = "0";
    Check(SeamlessSettings.StartupValue("party_guest_insight", "0") == "0" && SeamlessSettings.StartupDifferences(ReadRule).Contains("party_guest_insight"), "Insight 0 (the game's own) is not a difference");
}

// Host over a VPN (0.7.5 party_vpn) and the discovery adapter rule: the launcher repeats the game's adapter choice.
{
    NetAdapter Nic(string name, string ip, string mask = "255.255.255.0", bool gateway = false, bool physical = false) =>
        new(name, NetworkRoute.Parse(ip), NetworkRoute.Parse(mask), Gateway: gateway, Physical: physical);
    var lan = Nic("Ethernet / Realtek PCIe GbE", "192.168.1.20", gateway: true, physical: true);
    var radmin = Nic("Radmin VPN / Radmin VPN Ethernet Adapter", "26.10.20.30", "255.0.0.0");
    var hamachi = Nic("Hamachi / LogMeIn Hamachi Virtual Ethernet Adapter", "25.4.5.6", "255.0.0.0");
    var tailscale = Nic("Tailscale / Tailscale Tunnel", "100.101.102.103", "255.192.0.0");
    var zerotier = Nic("ZeroTier One / ZeroTier Virtual Port", "10.147.17.5", "255.255.255.0");
    var nord = Nic("NordLynx / NordLynx Tunnel", "10.5.0.2", "255.255.0.0");
    var hyperv = Nic("vEthernet (Default Switch) / Hyper-V Virtual Ethernet Adapter", "172.20.0.1", "255.255.240.0");
    var pub = Nic("Ethernet / Realtek PCIe GbE", "84.20.5.9", gateway: true, physical: true);
    Check(NetworkRoute.KindOf(radmin) == VpnKind.Radmin && NetworkRoute.KindOf(tailscale) == VpnKind.Tailscale && NetworkRoute.KindOf(nord) == VpnKind.Other && NetworkRoute.KindOf(lan) == VpnKind.None, "VPN kinds");
    // A virtual adapter whose name says nothing is told by its address range.
    Check(NetworkRoute.KindOf(Nic("Local Area Connection 3 / TAP-Windows Adapter V9", "26.1.1.1", "255.0.0.0")) == VpnKind.Radmin, "A TAP adapter in 26/8 is Radmin");
    var all = new[] { lan, zerotier, hamachi, nord, tailscale, radmin, hyperv };
    Check(NetworkRoute.SelectVpn(all, "auto") == radmin && NetworkRoute.SelectVpn(all, "") == radmin, "Auto should take Radmin first");
    Check(NetworkRoute.SelectVpn(new[] { lan, nord, hamachi, tailscale }, "auto") == hamachi, "Hamachi comes before Tailscale");
    Check(NetworkRoute.SelectVpn(new[] { lan, tailscale, zerotier }, "auto") == tailscale, "Tailscale comes before ZeroTier");
    Check(NetworkRoute.SelectVpn(new[] { lan, nord }, "auto") == null, "A commercial VPN was taken without being asked");
    Check(NetworkRoute.SelectVpn(all, "off") == null && NetworkRoute.SelectVpn(all, " Off ") == null, "Off still chose a VPN");
    Check(NetworkRoute.SelectVpn(all, "zerotier") == zerotier && NetworkRoute.SelectVpn(all, "nordlynx") == nord, "A named adapter was not taken");
    Check(NetworkRoute.SelectVpn(all, "nothing like it") == null, "An unknown name chose an adapter");
    var route = NetworkRoute.Choose(all, "auto", 47890);
    Check(route is { Label: "Radmin VPN", Connected: true, Address: "26.10.20.30:47890", Vpn: true }, "VPN route: " + route);
    Check(NetworkRoute.Choose(all, "auto", 0).Address == "26.10.20.30:9307", "A missing port should be the default port");
    route = NetworkRoute.Choose(all, "off", 9307);
    Check(route is { Label: "LAN", Address: "192.168.1.20:9307", Vpn: false }, "LAN route (VPN off): " + route);
    route = NetworkRoute.Choose(new[] { hyperv, pub }, "auto", 9307);
    Check(route is { Label: "Internet", Connected: true, Address: "" }, "A public address is the Internet route, no address to share: " + route);
    Check(!NetworkRoute.Choose(Array.Empty<NetAdapter>(), "auto", 9307).Connected, "No adapter is connected");
    Check(NetworkRoute.OpenNetworkWarning("host", " ", NetworkRoute.Choose(all, "auto", 9307)) &&
          !NetworkRoute.OpenNetworkWarning("host", "pw", NetworkRoute.Choose(all, "auto", 9307)) &&
          !NetworkRoute.OpenNetworkWarning("join", "", NetworkRoute.Choose(all, "auto", 9307)) &&
          !NetworkRoute.OpenNetworkWarning("host", "", NetworkRoute.Choose(new[] { lan }, "auto", 9307)), "Open network warning");

    // Discovery (party_discovery.h adapter_discovery_ok): private LAN on a real adapter, Radmin, Hamachi, ZeroTier.
    Check(NetworkRoute.DiscoveryOk(lan) && NetworkRoute.DiscoveryOk(radmin) && NetworkRoute.DiscoveryOk(hamachi) && NetworkRoute.DiscoveryOk(zerotier), "Allowed adapters refused");
    Check(!NetworkRoute.DiscoveryOk(tailscale) && !NetworkRoute.DiscoveryOk(nord) && !NetworkRoute.DiscoveryOk(hyperv) && !NetworkRoute.DiscoveryOk(pub), "Tailscale, a commercial VPN, a virtual switch or a public address was allowed");
    Check(!NetworkRoute.DiscoveryOk(lan with { Loopback = true }) && !NetworkRoute.DiscoveryOk(lan with { Tunnel = true }) && !NetworkRoute.DiscoveryOk(lan with { Ip = 0 }), "Loopback, tunnel or no address allowed");
    Check(NetworkRoute.DiscoveryOk(Nic("Radmin VPN / Radmin VPN Ethernet Adapter", "192.168.9.9")), "Radmin with a private address");
    Check(!NetworkRoute.DiscoveryOk(Nic("Radmin VPN / Radmin VPN Ethernet Adapter", "84.1.1.1")), "Radmin with a public address");
    var allowed = new[] { lan, radmin, tailscale, pub };
    Check(NetworkRoute.SourceAllowed(NetworkRoute.Parse("192.168.1.77"), allowed) && NetworkRoute.SourceAllowed(NetworkRoute.Parse("26.200.1.1"), allowed), "A host on an allowed subnet was dropped");
    Check(!NetworkRoute.SourceAllowed(NetworkRoute.Parse("192.168.2.77"), allowed) && !NetworkRoute.SourceAllowed(NetworkRoute.Parse("100.101.5.5"), allowed) &&
          !NetworkRoute.SourceAllowed(NetworkRoute.Parse("84.20.5.77"), allowed) && !NetworkRoute.SourceAllowed(NetworkRoute.Parse("8.8.8.8"), allowed), "A source outside the allowed subnets was taken");
    Check(NetworkRoute.Normalize(null) == "auto" && NetworkRoute.Normalize(" 0 ") == "off" && NetworkRoute.Normalize("Radmin") == "Radmin", "party_vpn normalisation");
    var heard = PartyDiscovery.Networks(new[] { lan, radmin, tailscale, lan with { Name = "Wi-Fi / Intel", Ip = NetworkRoute.Parse("192.168.5.2") } });
    Check(heard.Networks.SequenceEqual(new[] { "LAN", "Radmin VPN" }) && heard.Tailscale, "Join list networks or the Tailscale note");
    Check(PartyDiscovery.Networks(new[] { lan }) is { Tailscale: false, Networks: ["LAN"] }, "A LAN-only PC's join list networks");
    Check(NetworkRoute.Enumerate() != null, "Enumerate failed");
}

// Party discovery (party_discovery.h): the launcher's parser rejects what the game's parse() rejects.
{
    byte[] Packet(string build = "nightwatch-coop-0.2.4-0123456789abcdef", string name = "Hunter-1a2b", string area = "Central City",
        int players = 1, int max = 3, int port = 9307, int level = 42, int flags = 1, uint id = 0xdeadbeef, int version = 1)
    {
        var bytes = new List<byte> { 0xfa, (byte)'A', (byte)version, (byte)flags, (byte)(id >> 24), (byte)(id >> 16), (byte)(id >> 8), (byte)id,
            (byte)(port >> 8), (byte)port, (byte)(level >> 8), (byte)level, (byte)players, (byte)max };
        foreach (var s in new[] { build, name, area }) { bytes.Add((byte)s.Length); bytes.AddRange(System.Text.Encoding.Latin1.GetBytes(s)); }
        return bytes.ToArray();
    }
    var good = PartyDiscovery.Parse(Packet());
    Check(good is { Id: 0xdeadbeef, Port: 9307, Level: 42, Players: 1, Max: 3, Password: true, Name: "Hunter-1a2b", Area: "Central City" } &&
          good.Build == "nightwatch-coop-0.2.4-0123456789abcdef", "A valid announcement was not parsed");
    Check(PartyDiscovery.Parse(Packet(flags: 0))?.Password == false && PartyDiscovery.Parse(Packet(area: ""))?.Area == "", "No password flag or an empty area");
    Check(PartyDiscovery.Parse(Packet(build: new string('b', 64))) != null && PartyDiscovery.Parse(Packet(name: new string('n', 16), area: new string('a', 32))) != null, "The longest strings were refused");
    Check(PartyDiscovery.Parse(Packet(players: 3, max: 3)) != null && PartyDiscovery.Parse(Packet(max: 8)) != null, "A full party or 8 slots was refused");
    var bad = new Dictionary<string, byte[]>
    {
        ["empty"] = Array.Empty<byte>(), ["short"] = Packet()[..16], ["long"] = Packet(area: new string('a', 32)).Concat(new byte[100]).ToArray(),
        ["magic"] = new byte[] { 0xfb }.Concat(Packet().Skip(1)).ToArray(), ["version"] = Packet(version: 2), ["flags"] = Packet(flags: 2),
        ["port 0"] = Packet(port: 0), ["max 0"] = Packet(players: 0, max: 0), ["max 9"] = Packet(players: 1, max: 9), ["players > max"] = Packet(players: 4, max: 3),
        ["empty name"] = Packet(name: ""), ["build 65"] = Packet(build: new string('b', 65)), ["name 17"] = Packet(name: new string('n', 17)),
        ["area 33"] = Packet(area: new string('a', 33)), ["control"] = Packet(name: "Hunter"), ["del"] = Packet(name: "Hunter"),
        ["high byte"] = Packet(name: "Hunéter"), ["trailing"] = Packet().Concat(new byte[] { 0 }).ToArray(),
        ["overrun"] = Packet()[..^3], ["no area"] = Packet()[..^(1 + "Central City".Length)],
    };
    foreach (var (why, data) in bad)
        Check(PartyDiscovery.Parse(data) == null, "A bad announcement was taken: " + why);
    var overrun = Packet(); overrun[14 + 1 + "nightwatch-coop-0.2.4-0123456789abcdef".Length] = 200;   // the name's length byte runs past the end
    Check(PartyDiscovery.Parse(overrun) == null, "A length that overruns was taken");

    Check(PartyDiscovery.BuildMatches("nightwatch-coop-0.2.4-0123456789abcdef", PartyDiscovery.BuildPrefix(new Version(0, 2, 4, 7))) &&
          !PartyDiscovery.BuildMatches("nightwatch-coop-0.2.3-0123456789abcdef", PartyDiscovery.BuildPrefix(new Version(0, 2, 4))) &&
          !PartyDiscovery.BuildMatches("nightwatch-coop-0.2.40-abc", PartyDiscovery.BuildPrefix(new Version(0, 2, 4))) &&
          !PartyDiscovery.BuildMatches("0.7.5 (abc)", PartyDiscovery.BuildPrefix(new Version(0, 2, 4))), "Build match");

    // The Join list's rows: button state, the selected party, the Party size a Join sets.
    var prefix = PartyDiscovery.BuildPrefix(new Version(0, 2, 4));
    Check(PartyDiscovery.StateOf(good!, prefix) == PartyDiscovery.RowState.Join &&
          PartyDiscovery.StateOf(good! with { Players = 3 }, prefix) == PartyDiscovery.RowState.Full &&
          PartyDiscovery.StateOf(good! with { Players = 3, Build = "nightwatch-coop-0.2.3-ab" }, prefix) == PartyDiscovery.RowState.OtherBuild, "Join row state");
    var listed = new FoundParty(good!, "26.1.1.1", 0);
    Check(PartyDiscovery.Selected(listed, " 26.1.1.1:9307 ") && !PartyDiscovery.Selected(listed, "26.1.1.1:9308") && !PartyDiscovery.Selected(listed, ""), "Selected party");
    Check(PartyDiscovery.PartySize(3) == "3" && PartyDiscovery.PartySize(5) == "5" && PartyDiscovery.PartySize(8) == null && PartyDiscovery.PartySize(1) == null, "Party size from the announcement");

    var book = new PartyDiscovery.Book(new[] { "192.168.1.20" });
    Check(!book.Feed(Packet(), "192.168.1.20", 0), "The PC's own announcement was listed");
    Check(book.Feed(Packet(), "26.1.1.1", 0) && book.Feed(Packet(players: 2), "192.168.1.9", 100), "Valid announcements not taken");
    Check(book.Snapshot(200) is [{ Info.Players: 2, Ip: "192.168.1.9" }], "A game announcing on two adapters is listed twice or not updated");
    Check(book.Feed(Packet(id: 7, name: "Alpha"), "26.1.1.2", 300) && book.Snapshot(300).Select(p => p.Info.Name).SequenceEqual(new[] { "Alpha", "Hunter-1a2b" }), "Parties are not sorted by name");
    Check(book.Snapshot(300 + PartyDiscovery.TtlMs + 1).Count == 0, "Old announcements were kept");
    Check(!book.Feed(new byte[] { 1, 2, 3 }, "26.1.1.3", 400), "Garbage was taken");
    var flood = new PartyDiscovery.Book();
    var taken = Enumerable.Range(0, 60).Count(i => flood.Feed(Packet(id: (uint)i), "26.1.1.4", 5000 + i));
    Check(taken == PartyDiscovery.MaxPerSecond, "Rate limit per source: took " + taken);
    Check(flood.Feed(Packet(id: 1000), "26.1.1.4", 6500), "A source stayed limited in the next second");
    var many = new PartyDiscovery.Book();
    for (var i = 0; i < 100; i++) many.Feed(Packet(id: (uint)i), $"26.2.{i / 10}.{i % 10}", 10_000);
    Check(many.Snapshot(10_000).Count == PartyDiscovery.MaxEntries, "The list is not capped: " + many.Snapshot(10_000).Count);

    // The listener on a spare port: a datagram from this PC (loopback, so any source) reaches the list.
    var heard = new PartyDiscovery.Book();
    using (var listener = new PartyDiscovery.Listener(heard, () => Array.Empty<NetAdapter>(), 39308, anySource: true))
    using (var sender = new System.Net.Sockets.UdpClient())
    {
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline && heard.Snapshot(PartyDiscovery.NowMs).Count == 0)
        {
            var packet = Packet(id: 99, name: "Loop");
            sender.Send(packet, packet.Length, "127.0.0.1", 39308);
            Thread.Sleep(100);
        }
        Check(listener.Error == "" && heard.Snapshot(PartyDiscovery.NowMs) is [{ Info.Name: "Loop", Ip: "127.0.0.1" }], "The listener did not hear the announcement: " + listener.Error);
    }
    // Without anySource the same datagram is dropped (127.0.0.1 is on no allowed adapter).
    var strict = new PartyDiscovery.Book();
    using (var listener = new PartyDiscovery.Listener(strict, () => new[] { new NetAdapter("Ethernet / Realtek", NetworkRoute.Parse("192.168.1.20"), NetworkRoute.Parse("255.255.255.0")) }, 39309))
    using (var sender = new System.Net.Sockets.UdpClient())
    {
        for (var i = 0; i < 10; i++) { var packet = Packet(); sender.Send(packet, packet.Length, "127.0.0.1", 39309); Thread.Sleep(50); }
        Check(strict.Snapshot(PartyDiscovery.NowMs).Count == 0, "A datagram from outside the allowed subnets was listed");
    }
}

Console.WriteLine("PASS: host/join validation, start-up rules, VPN route, party discovery, password, save isolation, rule settings, base renderer options, package updates and the map's input with an update");

[System.Runtime.InteropServices.UnmanagedFunctionPointer(System.Runtime.InteropServices.CallingConvention.Cdecl)]
[return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.U1)]
delegate bool SdlInit(uint flags);
[System.Runtime.InteropServices.UnmanagedFunctionPointer(System.Runtime.InteropServices.CallingConvention.Cdecl)]
delegate void SdlQuit();
