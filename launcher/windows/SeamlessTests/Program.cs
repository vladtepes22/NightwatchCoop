using BloodborneLauncher.Core;

if (args.Length == 2 && args[0] == "--render-ui")
{
    UiRender.Check(args[1]);
    return;
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
Check(env["BB_PARTY_ITEMS"] == "1" && env["BB_PARTY_PROGRESS"] == "1", "Seamless defaults missing");
Check(env["BB_PARTY_AUTOCONTINUE"] == "0" && env["BB_PARTY_RESTARTED"] == "0", "Manual launch inherited automated Continue");
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
settings.Set("party_items", false);
SeamlessSettings.Apply(settings, env);
Check(env["BB_PARTY_ITEMS"] == "0", "Disabled rule ignored");
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
Console.WriteLine("PASS: host/join validation, password, save isolation, rule settings and base renderer options");
