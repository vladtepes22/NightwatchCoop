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
        public string Hand { get; set; } = "";
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
    internal sealed record Gem(long Handle, int Type, int Shape, int? Sort, bool Cursed, GemEffect[] Effects, Holder? SocketedIn = null, long Gen = 0);
    internal sealed record GemPreview(string? Name, string? Icon, bool Cursed);
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

    /// <summary>The Python side of one editor window, kept running so a slot or a save type opens without a new process
    /// and a new read of the game's catalogue. Requests go one at a time. A worker that died is restarted once for a read;
    /// a write is never sent twice, it reports the failure.</summary>
    internal sealed class Worker(LauncherSettings settings) : IDisposable
    {
        private readonly SemaphoreSlim _turn = new(1, 1);
        private Process? _process;

        private Process Start()
        {
            var start = new ProcessStartInfo(Paths.SeamlessPython)
            {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Paths.PortDir,
                RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true
            };
            start.Environment["PYTHONUTF8"] = "1";
            foreach (var argument in new[] { Path.Combine(Paths.SeamlessScripts, "saveedit_cli.py"), "worker",
                         "--user", GameEnvironment.UserDir(settings), "--kind", "solo",
                         "--game", GameEnvironment.GameDir(settings) }) start.ArgumentList.Add(argument);
            var process = Process.Start(start) ?? throw new IOException(L.T("Could not open the save editor data."));
            process.ErrorDataReceived += (_, _) => { };
            process.BeginErrorReadLine();
            return process;
        }

        private async Task<string?> Exchange(string request)
        {
            _process ??= Start();
            try
            {
                await _process.StandardInput.WriteLineAsync(request);
                await _process.StandardInput.FlushAsync();
                return await _process.StandardOutput.ReadLineAsync();
            }
            catch (Exception e) when (e is IOException or ObjectDisposedException or InvalidOperationException) { return null; }
        }

        internal async Task<string> Run(string command, string kind, int? slot = null, object? edit = null, string? backup = null)
        {
            var writes = command is "apply" or "restore";
            if (writes)
            {
                var games = ProcessTree.GamesIn(Paths.EngineDir);
                var running = games.Length > 0;
                foreach (var game in games) game.Dispose();
                if (Workspace.Current.Game.Running || running)
                    throw new InvalidOperationException(L.T("Close the game before changing saves."));
            }
            var request = JsonSerializer.Serialize(new { command, kind, slot, backup, edit }, Json);
            await _turn.WaitAsync();
            try
            {
                var line = await Exchange(request);
                if (line == null)
                {
                    Stop();
                    if (writes) throw new IOException(L.T("Could not open the save editor data."));
                    line = await Exchange(request) ?? throw new IOException(L.T("Could not open the save editor data."));
                }
                using var reply = JsonDocument.Parse(line);
                if (reply.RootElement.TryGetProperty("result", out var result)) return result.GetRawText();
                var type = reply.RootElement.TryGetProperty("type", out var value) ? value.GetString() : "";
                throw new InvalidOperationException(type switch
                {
                    "GameRunning" => L.T("Close the game before changing saves."),
                    "EditConflict" => L.T("The save changed. Open the slot again before applying edits."),
                    _ => L.T("Could not complete the save operation. Check the values and save folder, then try again.")
                });
            }
            finally { _turn.Release(); }
        }

        private void Stop()
        {
            if (_process == null) return;
            try
            {
                _process.StandardInput.Close();  // it exits on the closed pipe
                if (!_process.WaitForExit(1000)) _process.Kill();
            }
            catch (Exception e) when (e is IOException or InvalidOperationException or System.ComponentModel.Win32Exception) { }
            _process.Dispose();
            _process = null;
        }

        public void Dispose() => Stop();
    }
}
