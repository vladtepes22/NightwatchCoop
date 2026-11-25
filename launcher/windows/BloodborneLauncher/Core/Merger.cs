using System.Diagnostics;
using System.IO;
using System.Text;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The mod merger (tools\merger): the package's Python source in merger\source, run by the package's own Python like the
    /// generator. It reads the chosen mods (later ones win a clash) and writes a new mod folder; it never enables it.
    /// </summary>
    public static class Merger
    {
        public static string Script => Path.Combine(Paths.PortDir, "merger", "source", "merger.py");

        /// <summary>This package has the merger and a Python to run it (the Merge tab stays hidden without them).</summary>
        public static bool Supported => File.Exists(Script) && File.Exists(Paths.SeamlessPython);

        /// <summary>The log of the last merge, in the launcher's logs folder (the tool overwrites it each run).</summary>
        public static string LogFile => Path.Combine(Path.GetDirectoryName(GameRunner.SessionLog)!, "merge.log");

        /// <summary>The longest name the Merge page accepts: the tool's files sit inside the folder (Windows' 259-character paths).</summary>
        public const int MaxName = 60;

        /// <summary>Whether the text can be the new mod's folder name; otherwise false, and the reason (empty for no text).</summary>
        public static bool ValidName(string name, string modsDir, out string error)
        {
            error = "";
            var trimmed = name.Trim();
            if (trimmed.Length == 0) return false;
            if (trimmed.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || trimmed.EndsWith('.') || trimmed.StartsWith('.') ||
                trimmed.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase) || Randomizer.IsWorld(trimmed) || IsReserved(trimmed))
            {
                error = L.T("That cannot be a folder name.");
                return false;
            }
            if (trimmed.Length > MaxName)
            {
                error = L.T("That name is too long.");
                return false;
            }
            if (Directory.Exists(Path.Combine(modsDir, trimmed)) || File.Exists(Path.Combine(modsDir, trimmed)))
            {
                error = L.T("A mod with that name already exists.");
                return false;
            }
            return true;
        }

        private static bool IsReserved(string name)
        {
            var stem = name.Split('.')[0].TrimEnd().ToUpperInvariant();
            return stem is "CON" or "PRN" or "AUX" or "NUL" || (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) && char.IsDigit(stem[3]));
        }

        /// <summary>The arguments after the script: the fixed command line of merger.py.</summary>
        public static List<string> Arguments(string gameDir, string output, string log, IEnumerable<string> modDirs)
        {
            var arguments = new List<string> { "merge", "--game", gameDir, "--out", output, "--log", log };
            foreach (var dir in modDirs)
            {
                arguments.Add("--mod");
                arguments.Add(dir);
            }
            return arguments;
        }

        /// <summary>Merges the mods (folder names in the Mods folder, in merge order) into a new mod called name. Each stdout line
        /// goes to line (on the caller's context). Returns null when it worked, else the tool's error text.</summary>
        public static async Task<string?> Merge(LauncherSettings s, string name, IReadOnlyList<string> mods,
            Action<string>? line = null, CancellationToken cancel = default)
        {
            var root = GameEnvironment.ModsDir(s);
            try
            {
                Directory.CreateDirectory(root);
                Directory.CreateDirectory(Path.GetDirectoryName(LogFile)!);
                var info = new ProcessStartInfo(Paths.SeamlessPython)
                {
                    WorkingDirectory = Path.GetDirectoryName(Script)!,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8,
                };
                // -I: nothing of this PC's Python settings; -B: no __pycache__ in the package; UTF-8 for the names it prints
                var all = new[] { "-I", "-B", "-X", "utf8", Script }.Concat(
                    Arguments(GameEnvironment.GameDir(s), Path.Combine(root, name.Trim()), LogFile, mods.Select(m => Path.Combine(root, m))));
                foreach (var argument in all) info.ArgumentList.Add(argument);
                cancel.ThrowIfCancellationRequested();
                using var process = Process.Start(info)!;
                using var stop = cancel.Register(() =>
                {
                    try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { }
                });
                var errors = process.StandardError.ReadToEndAsync();
                while (await process.StandardOutput.ReadLineAsync() is { } text)
                    line?.Invoke(text);
                await process.WaitForExitAsync();
                cancel.ThrowIfCancellationRequested();
                if (process.ExitCode == 0) return null;
                var last = (await errors).Split('\n').Select(l => l.Trim()).LastOrDefault(l => l.Length > 0);
                return last ?? L.T("The merge failed.");
            }
            catch (OperationCanceledException)
            {
                RemoveUnfinished(root, name.Trim());
                return L.T("The merge was stopped.");
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                return e.Message;
            }
        }

        /// <summary>The tool writes "&lt;name&gt;.tmp" and renames it when done; a stopped run leaves that folder behind.</summary>
        private static void RemoveUnfinished(string root, string name)
        {
            try { Directory.Delete(Path.Combine(root, name + ".tmp"), recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }
}
