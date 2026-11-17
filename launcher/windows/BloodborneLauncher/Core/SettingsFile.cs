using System.IO;
using System.Text;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// How the launcher writes its settings files (settings.json, bbport.ini, mods.json, patches.json): never over a file
    /// that exists but could not be read or parsed, through a temp file in the same folder, the last good file kept as
    /// <c>.bak</c>. A refused write is reported once per file through <see cref="Warned"/> (the launcher log).
    /// </summary>
    public static class SettingsFile
    {
        /// <summary>A write was refused or failed in a way the player should know (text for the launcher log).</summary>
        public static event Action<string>? Warned;

        private static readonly object Gate = new();
        private static readonly HashSet<string> Told = new(StringComparer.OrdinalIgnoreCase);

        /// <summary>The file's text, null when it does not exist; <paramref name="failed"/> when it exists but could not be
        /// read (a few tries: the game or an editor may hold it for a moment).</summary>
        public static string? ReadText(string path, out bool failed)
        {
            failed = false;
            for (var attempt = 0; ; attempt++)
            {
                try
                {
                    return File.Exists(path) ? File.ReadAllText(path) : null;
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    if (attempt >= 2) { failed = true; return null; }
                    Thread.Sleep(40);
                }
            }
        }

        /// <summary>JSON text that holds an object.</summary>
        public static bool IsJsonObject(string text)
        {
            try { return JsonNode.Parse(text) is JsonObject; }
            catch { return false; }
        }

        /// <summary>True when a write may replace the file: it is missing, empty, or readable (and, for JSON, an object).
        /// False for a file that exists but cannot be read or parsed: its content may be all the player has.</summary>
        public static bool CanReplace(string path, bool json)
        {
            var text = ReadText(path, out var failed);
            if (failed) return false;
            if (text == null || text.Trim().Length == 0) return true;
            return !json || IsJsonObject(text);
        }

        /// <summary>Writes <paramref name="text"/> to <paramref name="path"/>: refused (false, told once) when the file there
        /// cannot be read; otherwise a temp file beside it is moved over it and the previous content stays as
        /// <c>path.bak</c> (only a readable, non-empty previous file, so a broken one never replaces the good copy).</summary>
        public static bool Write(string path, string text, bool json)
        {
            lock (Gate)
            {
                var old = ReadText(path, out var failed);
                if (failed || (old != null && old.Trim().Length > 0 && json && !IsJsonObject(old)))
                {
                    Warn(path, failed ? "could not be read" : "is not valid and was kept as it is");
                    return false;
                }
                Told.Remove(path);

                var temp = path + ".tmp";
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
                    File.WriteAllText(temp, text, new UTF8Encoding(false));
                    if (old != null && old.Trim().Length > 0)
                    {
                        // The backup is a convenience: a .bak that cannot be written must not stop the save.
                        try { File.Copy(path, path + ".bak", true); }
                        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
                    }
                    File.Move(temp, path, true);
                    return true;
                }
                catch
                {
                    try { File.Delete(temp); } catch { /* best effort */ }
                    throw;
                }
            }
        }

        /// <summary>Tells the launcher log once per file (until a write to it succeeds again) that a save was refused.</summary>
        public static void Warn(string path, string why)
        {
            string name = Path.GetFileName(path);
            lock (Gate)
                if (!Told.Add(path)) return;
            Warned?.Invoke($"{name} {why}: your changes to it are not saved (the file was not overwritten; the last good copy, if any, is {name}.bak).");
        }
    }
}
