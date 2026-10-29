using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace BloodborneLauncher.Core;

internal static class SaveEditor
{
    internal static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };
    internal sealed record Slot([property: JsonPropertyName("slot")] int SlotNumber, string Name, int Level, int Echoes)
    {
        public override string ToString() => $"{SlotNumber + 1}: {Name} ({Level}, {Echoes})";
    }
    internal sealed record Character(string Name, Dictionary<string, int> Stats, int Level, int Echoes, int Insight, int NgCycle);
    internal sealed class Item
    {
        public int Section { get; set; }
        public int Slot { get; set; }
        public string Kind { get; set; } = "";
        public int Number { get; set; }
        public string? Name { get; set; }
        public int Count { get; set; }
        public int? Level { get; set; }
        public int? MaxLevel { get; set; }
        // A PNG cut from the player's own game files (null when the game folder or the icon is missing).
        public string? Icon { get; set; }
        // A gem sitting in a weapon slot: shown, never counted or removed.
        public bool Locked { get; set; }
        public Gem? Gem { get; set; }
        public Socket[]? Sockets { get; set; }
    }
    // Type 1 blood gem, 2 rune; Sort 0 blood gem, 1 Caryll rune, 2 oath rune. Handles are unsigned 32-bit.
    internal sealed record Gem(long Handle, int Type, int Shape, int? Sort, bool Cursed, GemEffect[] Effects, Holder? SocketedIn = null);
    internal sealed record GemEffect(int Id, string? Text);
    internal sealed record Holder(int? Number, string? Name);
    internal sealed record Socket(int Shape, string? Gem);
    internal sealed record Flag(int Id, string Kind, string Group, string Label, bool On);
    internal sealed record Findings(bool Phantom, string[] DuplicateKeys, bool WorldEmpty);
    internal sealed record Snapshot(string Revision, Character Character, Item[] Items, Flag[]? Flags = null, Findings? Findings = null);
    internal sealed record Backup(string Name, string Path, string Reason)
    {
        public override string ToString() => Name;
    }
    internal sealed record WriteResult(string? Backup);

    internal static T Parse<T>(string text) => JsonSerializer.Deserialize<T>(text, Json)
        ?? throw new JsonException();

    internal static async Task<T> Run<T>(LauncherSettings settings, string command, string kind,
        int? slot = null, object? edit = null, string? backup = null)
    {
        if (command is "apply" or "restore")
        {
            var games = ProcessTree.GamesIn(Paths.EngineDir);
            var running = games.Length > 0;
            foreach (var game in games) game.Dispose();
            if (Workspace.Current.Game.Running || running)
                throw new InvalidOperationException(L.T("Close the game before changing saves."));
        }
        var start = new ProcessStartInfo(Paths.SeamlessPython)
        {
            UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Paths.PortDir,
            RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true
        };
        start.Environment["PYTHONUTF8"] = "1";
        foreach (var argument in new[] { Path.Combine(Paths.SeamlessScripts, "saveedit_cli.py"), command,
                     "--user", GameEnvironment.UserDir(settings), "--kind", kind,
                     "--game", GameEnvironment.GameDir(settings) }) start.ArgumentList.Add(argument);
        if (slot.HasValue) { start.ArgumentList.Add("--slot"); start.ArgumentList.Add(slot.Value.ToString()); }
        if (backup != null) { start.ArgumentList.Add("--backup"); start.ArgumentList.Add(backup); }
        using var process = Process.Start(start) ?? throw new IOException(L.T("Could not open the save editor data."));
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        if (edit != null) await process.StandardInput.WriteAsync(JsonSerializer.Serialize(edit, Json));
        process.StandardInput.Close();
        await process.WaitForExitAsync();
        var text = await output;
        await error;
        if (process.ExitCode != 0)
        {
            using var failure = JsonDocument.Parse(text);
            var type = failure.RootElement.TryGetProperty("type", out var value) ? value.GetString() : "";
            throw new InvalidOperationException(type switch
            {
                "GameRunning" => L.T("Close the game before changing saves."),
                "EditConflict" => L.T("The save changed. Open the slot again before applying edits."),
                _ => L.T("Could not complete the save operation. Check the values and save folder, then try again.")
            });
        }
        return Parse<T>(text);
    }
}
