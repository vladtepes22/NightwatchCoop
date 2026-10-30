using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace BloodborneLauncher.Core;

public static class SeamlessSession
{
    public sealed record Member(string name, bool connected, bool in_world, bool local, int ping_ms, string area);
    public sealed record Status(string role, string state, string detail, string network, string @event, string alert, string guest_code, Member[] members);

    public static Status? Read(LauncherSettings settings)
    {
        var path = Path.Combine(GameEnvironment.UserDir(settings), "party_status.json");
        try
        {
            if (!File.Exists(path) || DateTime.UtcNow - File.GetLastWriteTimeUtc(path) > TimeSpan.FromSeconds(5)) return null;
            return JsonSerializer.Deserialize<Status>(File.ReadAllText(path));
        }
        catch (Exception e) when (e is IOException or JsonException) { return null; }
    }

    public static void Command(LauncherSettings settings, string action, string name = "")
    {
        if (!Workspace.Current.Game.Running) throw new InvalidOperationException("Start the game first.");
        PostCommand(GameEnvironment.UserDir(settings), action, name);
    }

    internal static void PostCommand(string user, string action, string name = "")
    {
        var path = Path.Combine(user, "party_command.json");
        File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(new { action, name }));
        File.Move(path + ".tmp", path, true);
    }

    public static async Task<string> SaveTool(LauncherSettings settings, string action, params string[] extra)
    {
        var games = ProcessTree.GamesIn(Paths.EngineDir);
        var running = games.Length > 0;
        foreach (var game in games) game.Dispose();
        if (Workspace.Current.Game.Running || running)
            throw new InvalidOperationException("Stop the game before checking or changing saves.");
        return await RunSaveTool(GameEnvironment.UserDir(settings), action, extra);
    }

    /// <summary>Runs party_save_tool.py `action` on the user folder `user` (the game must not run).</summary>
    internal static Task<string> RunSaveTool(string user, string action, params string[] extra) =>
        RunScript("party_save_tool.py", user, action, extra);

    /// <summary>party_offline_tool.py `action` (list, copy): a party character into the single-player folder. The game must not run.</summary>
    public static async Task<string> OfflineTool(LauncherSettings settings, string action, params string[] extra)
    {
        var games = ProcessTree.GamesIn(Paths.EngineDir);
        var running = games.Length > 0;
        foreach (var game in games) game.Dispose();
        if (Workspace.Current.Game.Running || running)
            throw new InvalidOperationException("Stop the game before checking or changing saves.");
        return await RunScript("party_offline_tool.py", GameEnvironment.UserDir(settings), action, extra);
    }

    private static async Task<string> RunScript(string script, string user, string action, string[] extra)
    {
        var start = new ProcessStartInfo(Paths.SeamlessPython)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            WorkingDirectory = Paths.PortDir
        };
        start.Environment["PYTHONUTF8"] = "1";
        start.ArgumentList.Add(Path.Combine(Paths.SeamlessScripts, script));
        start.ArgumentList.Add(action);
        start.ArgumentList.Add(user);
        foreach (var argument in extra) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new IOException("Could not start the save tool.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var text = await output;
        var failure = await error;
        if (process.ExitCode != 0) throw new IOException(failure.Trim());
        return text.Trim();
    }
}
