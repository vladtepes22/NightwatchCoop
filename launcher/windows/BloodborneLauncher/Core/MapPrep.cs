using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The in-game map's files, made once on this PC from the player's own game (never shipped):
    /// the package's map generator (map-generator\, Hunter's Atlas's, GPL) writes the map data,
    /// then bb-map-bake turns it into what the engine draws. Runs in the background; the game can be
    /// played meanwhile and turns its map on by itself when the baked folder appears (BB_MAP_DIR,
    /// engine gpu/shim/map/map_page.cpp), so nobody restarts. Kept in the user folder
    /// (the updater keeps user\), redone when the game folder changes.
    /// </summary>
    public static class MapPrep
    {
        public enum Phase { Off, Waiting, Preparing, Ready, Failed }

        public static Phase State { get; private set; } = Phase.Off;
        public static int Percent { get; private set; }
        public static string Step { get; private set; } = "";
        public static string Error { get; private set; } = "";
        /// <summary>UI thread (Workspace.Dispatch): the state changed.</summary>
        public static event Action? Changed;

        private static Process? _running;
        private static Task? _task;

        public static string GeneratorDir => Path.Combine(Paths.PortDir, "map-generator");
        public static string Generator => Path.Combine(GeneratorDir, "generate_atlas_lite.exe");
        public static string Parchment => Path.Combine(GeneratorDir, "parchment.bbra");
        public static string Baker => Path.Combine(Paths.BinariesDir, Paths.Program("bb-map-bake"));

        /// <summary>This package can make maps (an older or custom package has no generator).</summary>
        public static bool Supported => File.Exists(Generator) && File.Exists(Baker) && File.Exists(Parchment);

        /// <summary>The player turned the in-game map on (launcher Display › Map; on by default since 0.2.4).</summary>
        public static bool Enabled(LauncherSettings s) => Supported && s.Bool("map_enabled");

        /// <summary>The Display page's switch: on makes the maps (when missing), off stops making them;
        /// the game gets or loses its map at its next start.</summary>
        public static void SetEnabled(LauncherSettings s, bool on)
        {
            s.Set("map_enabled", on);
            if (on)
            {
                Start(s);
                return;
            }
            Stop();
            Set(Phase.Off, 0, "");
        }

        /// <summary>One set for all player profiles: the maps are the game's, not a player's.</summary>
        public static string Root(LauncherSettings s) => Path.Combine(GameEnvironment.SavesRoot(s), "maps");
        private static string Atlas(LauncherSettings s) => Path.Combine(Root(s), "atlas");
        /// <summary>The engine's BB_MAP_DIR: complete once it exists (bb-map-bake renames it into place).</summary>
        public static string Baked(LauncherSettings s) => Path.Combine(Root(s), "baked");
        private static string Stamp(LauncherSettings s) => Path.Combine(Root(s), "made-from.txt");

        /// <summary>Ready for this game folder: baked from it, nothing left to do.</summary>
        public static bool IsReady(LauncherSettings s) =>
            File.Exists(Path.Combine(Baked(s), "atlas2-index.json")) &&
            File.Exists(Stamp(s)) && File.ReadAllText(Stamp(s)).Trim() == Source(s);

        private static string Source(LauncherSettings s) => Path.GetFullPath(GameEnvironment.GameDir(s));

        /// <summary>The engine's map variables: the folder the maps go to, also before they are there.</summary>
        public static void AddEnvironment(LauncherSettings s, IDictionary<string, string> env)
        {
            if (!Enabled(s) || GameEnvironment.GameDir(s).Length == 0) return;
            env["BB_MAP_DIR"] = Baked(s);
            env["BB_MAP_PARCHMENT"] = Parchment;
        }

        /// <summary>Starts making the maps when they are missing (game folder checked good first).</summary>
        public static void Start(LauncherSettings s)
        {
            if (!Enabled(s) || _task is { IsCompleted: false }) return;
            if (IsReady(s))
            {
                Set(Phase.Ready, 100, "");
                return;
            }
            Error = "";
            Set(Phase.Preparing, 0, L.T("Starting"));
            var game = Source(s);
            var root = Root(s);
            var atlas = Atlas(s);
            var baked = Baked(s);
            var stamp = Stamp(s);
            _task = Task.Run(() => Make(game, root, atlas, baked, stamp));
        }

        /// <summary>The launcher closes: the run stops (it starts over next time).</summary>
        public static void Stop()
        {
            try { _running?.Kill(entireProcessTree: true); } catch { /* already gone */ }
        }

        private static async Task Make(string game, string root, string atlas, string baked, string stamp)
        {
            var ws = Workspace.Current;
            try
            {
                Directory.CreateDirectory(root);
                File.Delete(stamp);
                // The generator's data stays when only the bake was interrupted.
                var generated = File.Exists(Path.Combine(atlas, "maps", "generated-assets.json")) &&
                                File.Exists(stamp + ".generated") && File.ReadAllText(stamp + ".generated").Trim() == game;
                if (!generated)
                {
                    ws.Log(L.T("Map: making the maps from your game files (once, a few minutes; you can play meanwhile)"), LogLevel.Step);
                    if (Directory.Exists(atlas)) Directory.Delete(atlas, recursive: true);
                    File.Delete(stamp + ".generated");
                    var generator = new ProcessStartInfo(Generator) { WorkingDirectory = GeneratorDir };
                    var input = PrepareInput(game, root);
                    generator.ArgumentList.Add("--game-dir");
                    generator.ArgumentList.Add(input);
                    generator.ArgumentList.Add("--output-dir");
                    generator.ArgumentList.Add(atlas);
                    generator.Environment["ATLAS_WORKERS"] = Workers().ToString();
                    try
                    {
                        await Run(generator, line =>
                        {
                            // {"progress": 34, "stage": "Reading collision maps (5/16)", ...}
                            if (!line.StartsWith('{')) return;
                            try
                            {
                                using var doc = JsonDocument.Parse(line);
                                var e = doc.RootElement;
                                if (e.TryGetProperty("stage", out var stage) && e.TryGetProperty("message", out var message) &&
                                    stage.GetString() == "error")
                                    Error = message.GetString() ?? "";
                                else if (e.TryGetProperty("progress", out var p) && stage.ValueKind == JsonValueKind.String)
                                    Set(Phase.Preparing, p.GetInt32() * 70 / 100, stage.GetString() ?? "");
                            }
                            catch (JsonException) { /* not a progress line */ }
                        });
                    }
                    finally
                    {
                        // Without an update the input is the game folder itself: never removed.
                        if (input != game) RemoveInput(input);
                    }
                    File.WriteAllText(stamp + ".generated", game);
                    // The generator's own scratch space, not needed after a run.
                    try { Directory.Delete(Path.Combine(atlas, ".generation-staging"), recursive: true); } catch { /* none */ }
                }

                Set(Phase.Preparing, 70, L.T("Preparing the map for the game"));
                if (Directory.Exists(baked)) Directory.Delete(baked, recursive: true);
                var areas = 0;
                var baker = new ProcessStartInfo(Baker) { WorkingDirectory = Paths.BinariesDir };
                baker.ArgumentList.Add(Path.Combine(atlas, "maps"));
                baker.ArgumentList.Add(baked);
                baker.ArgumentList.Add(Workers().ToString());
                await Run(baker, line =>
                {
                    if (line.StartsWith("MAP_AREA_READY", StringComparison.Ordinal))
                        Set(Phase.Preparing, 70 + Math.Min(29, ++areas * 29 / 16), L.F($"Preparing the map for the game ({areas}/16)"));
                    else if (line.StartsWith("MAP_BAKE_ERROR:", StringComparison.Ordinal))
                        Error = line["MAP_BAKE_ERROR:".Length..].Trim();
                });
                File.WriteAllText(stamp, game);
                Set(Phase.Ready, 100, "");
                ws.Log(ws.Game.Running
                        ? L.T("Map ready. It is on in the running game now (no restart): hold Options / Start or press M.")
                        : L.T("Map ready: in the game, hold Options / Start or press M."), LogLevel.Good);
            }
            catch (Exception e)
            {
                if (State == Phase.Off) return;  // turned off while it was making them
                if (Error.Length == 0) Error = e.Message;
                Set(Phase.Failed, 0, "");
                ws.Log(L.T("Map: the maps could not be made: ") + Error, LogLevel.Warn);
            }
        }

        /// <summary>The folder the generator reads. With the 1.09 update beside the game (GameCheck finds
        /// it, as the engine lays it over the game at start), it is a view of both, as the engine sees
        /// them: the update's eboot.bin and sce_sys, and its dvdroot_ps4 files over the game's. The update
        /// brings maps the game folder lacks (on base dumps such as CUSA00900, The Old Hunters' three
        /// areas), so the game's dvdroot_ps4 alone is not enough. Folders only one side has are linked
        /// (junctions); files in folders both have are hard links (copies across drives). The game and
        /// update folders are never written. Without an update it is the game folder.</summary>
        public static string PrepareInput(string game, string root)
        {
            var update = GameCheck.UpdateFolder(game);
            if (update == null) return game;
            var input = Path.Combine(root, "input");
            RemoveInput(input);
            Directory.CreateDirectory(Path.Combine(input, "sce_sys"));
            File.WriteAllText(Path.Combine(input, InputMarker), "");
            var updateData = Path.Combine(update, "dvdroot_ps4");
            if (Directory.Exists(updateData))
                Overlay(Path.Combine(game, "dvdroot_ps4"), updateData, Path.Combine(input, "dvdroot_ps4"));
            else
                LinkFolder(Path.Combine(game, "dvdroot_ps4"), Path.Combine(input, "dvdroot_ps4"));
            File.Copy(Path.Combine(update, "eboot.bin"), Path.Combine(input, "eboot.bin"), overwrite: true);
            var sfo = File.Exists(Path.Combine(update, "sce_sys", "param.sfo"))
                ? Path.Combine(update, "sce_sys", "param.sfo")
                : Path.Combine(game, "sce_sys", "param.sfo");
            File.Copy(sfo, Path.Combine(input, "sce_sys", "param.sfo"), overwrite: true);
            return input;
        }

        /// <summary>Marks a folder PrepareInput made, so RemoveInput never deletes anything else.</summary>
        private const string InputMarker = "nightwatch-map-input.txt";

        /// <summary><paramref name="view"/> = <paramref name="top"/> laid over <paramref name="bottom"/>.</summary>
        private static void Overlay(string bottom, string top, string view)
        {
            Directory.CreateDirectory(view);
            var bottomDirs = Directory.GetDirectories(bottom).ToDictionary(p => Path.GetFileName(p), StringComparer.OrdinalIgnoreCase);
            var topDirs = Directory.GetDirectories(top).ToDictionary(p => Path.GetFileName(p), StringComparer.OrdinalIgnoreCase);
            foreach (var (name, dir) in topDirs)
                if (bottomDirs.TryGetValue(name, out var under)) Overlay(under, dir, Path.Combine(view, name));
                else LinkFolder(dir, Path.Combine(view, name));
            foreach (var (name, dir) in bottomDirs)
                if (!topDirs.ContainsKey(name)) LinkFolder(dir, Path.Combine(view, name));
            var files = Directory.GetFiles(bottom).ToDictionary(p => Path.GetFileName(p), StringComparer.OrdinalIgnoreCase);
            foreach (var file in Directory.GetFiles(top)) files[Path.GetFileName(file)] = file;
            foreach (var (name, file) in files)
            {
                var link = Path.Combine(view, name);
                if (!CreateHardLinkW(link, file, IntPtr.Zero)) File.Copy(file, link);
            }
        }

        [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
        private static extern bool CreateHardLinkW(string link, string existing, IntPtr security);

        /// <summary>Removes a view made by PrepareInput: links and hard links go, the game's and the
        /// update's files stay (never followed into a junction). Anything else, the game folder
        /// itself included, is left alone.</summary>
        public static void RemoveInput(string input)
        {
            if (!Directory.Exists(input)) return;
            // An older launcher's view: no marker, its dvdroot_ps4 a junction.
            var data = new DirectoryInfo(Path.Combine(input, "dvdroot_ps4"));
            if (!File.Exists(Path.Combine(input, InputMarker)) &&
                !(data.Exists && data.Attributes.HasFlag(FileAttributes.ReparsePoint))) return;
            RemoveView(new DirectoryInfo(input));
        }

        private static void RemoveView(DirectoryInfo dir)
        {
            if (dir.Attributes.HasFlag(FileAttributes.ReparsePoint))
            {
                dir.Delete(recursive: false);  // the junction only
                return;
            }
            foreach (var sub in dir.EnumerateDirectories()) RemoveView(sub);
            foreach (var file in dir.EnumerateFiles()) file.Delete();
            dir.Delete(recursive: false);
        }

        private static void LinkFolder(string target, string link)
        {
            var mklink = new ProcessStartInfo("cmd.exe") { UseShellExecute = false, CreateNoWindow = true };
            foreach (var arg in new[] { "/c", "mklink", "/J", link, target }) mklink.ArgumentList.Add(arg);
            using var process = Process.Start(mklink) ?? throw new IOException("Cannot start mklink for the map");
            process.WaitForExit();
            if (process.ExitCode != 0) throw new IOException("Cannot link the game's files for the map: " + target);
        }

        /// <summary>Runs a step at low priority (the game may be playing), its output lines to
        /// <paramref name="line"/>; throws when it fails.</summary>

        private static async Task Run(ProcessStartInfo info, Action<string> line)
        {
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.RedirectStandardOutput = true;
            info.RedirectStandardError = true;
            info.StandardOutputEncoding = Encoding.UTF8;
            info.StandardErrorEncoding = Encoding.UTF8;
            using var process = new Process { StartInfo = info };
            var tail = new Queue<string>();
            process.OutputDataReceived += (_, e) => { if (e.Data != null) line(e.Data); };
            process.ErrorDataReceived += (_, e) =>
            {
                if (e.Data == null) return;
                lock (tail)
                {
                    tail.Enqueue(e.Data);
                    if (tail.Count > 5) tail.Dequeue();
                }
            };
            process.Start();
            _running = process;
            // Its workers inherit this: the game keeps the CPU when it needs it.
            try { process.PriorityClass = ProcessPriorityClass.BelowNormal; } catch { /* exited already */ }
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync();
            _running = null;
            if (process.ExitCode != 0)
            {
                string last;
                lock (tail) last = tail.LastOrDefault(l => l.Trim().Length > 0) ?? "";
                throw new InvalidOperationException(
                    Error.Length > 0 ? Error : L.F($"{Path.GetFileName(info.FileName)} stopped with code {process.ExitCode}. {last}"));
            }
        }

        /// <summary>The generator's worker processes: by CPU, at most one per 1.5 GB of memory
        /// (each holds a map's collision), fewer while the game runs.</summary>
        private static int Workers()
        {
            var gb = GC.GetGCMemoryInfo().TotalAvailableMemoryBytes / (1024.0 * 1024 * 1024);
            var byMemory = (int)((gb - 4) / 1.5);
            var byCpu = Environment.ProcessorCount - 2;
            if (Workspace.Current.Game.Running) byCpu = Environment.ProcessorCount / 4;
            return Math.Clamp(Math.Min(byMemory, byCpu), 1, 14);
        }

        private static void Set(Phase phase, int percent, string step)
        {
            State = phase;
            Percent = percent;
            Step = step;
            Workspace.Dispatch(() => Changed?.Invoke());
        }
    }
}
