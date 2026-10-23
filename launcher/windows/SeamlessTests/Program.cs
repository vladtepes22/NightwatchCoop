using BloodborneLauncher.Core;
using System.IO;

if (args.Length == 2 && args[0] == "--render-ui")
{
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

Console.WriteLine("PASS: host/join validation, password, save isolation, rule settings, base renderer options and package updates");

[System.Runtime.InteropServices.UnmanagedFunctionPointer(System.Runtime.InteropServices.CallingConvention.Cdecl)]
[return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.U1)]
delegate bool SdlInit(uint flags);
[System.Runtime.InteropServices.UnmanagedFunctionPointer(System.Runtime.InteropServices.CallingConvention.Cdecl)]
delegate void SdlQuit();
