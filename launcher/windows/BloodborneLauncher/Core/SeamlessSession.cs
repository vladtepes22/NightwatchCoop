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

    public static async Task<string> SaveTool(LauncherSettings settings, string action)
    {
        var games = ProcessTree.GamesIn(Paths.PortDir);
        var running = games.Length > 0;
        foreach (var game in games) game.Dispose();
        if (Workspace.Current.Game.Running || running)
            throw new InvalidOperationException("Stop the game before checking or changing saves.");
        var start = new ProcessStartInfo(File.Exists(Paths.Python) ? Paths.Python : "py")
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            WorkingDirectory = Paths.PortDir
        };
        start.Environment["PYTHONUTF8"] = "1";
        start.ArgumentList.Add(Path.Combine(Paths.PortDir, "scripts", "party_save_tool.py"));
        start.ArgumentList.Add(action);
        start.ArgumentList.Add(GameEnvironment.UserDir(settings));
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
