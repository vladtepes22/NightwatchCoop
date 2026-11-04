using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;

namespace BloodborneLauncher.Core
{
    /// <summary>This PC's party mods and patches hashes, first 8 hex each, as a game start would have them now (user,
    /// 4 Nov: the lobby compared against the last start's hash, which was stale after a mods or patches change). Asked
    /// from bb-start --party-hashes, the start's own code, whenever the inputs' fingerprint changes; while a game of this
    /// install runs, read from the files that game wrote. Null halves: unknown.</summary>
    public static class PartyHashes
    {
        public readonly record struct Hashes(string? Mods, string? Patches);

        private static readonly object Gate = new();
        private static Hashes _current;
        private static string? _fingerprint;  // of the inputs _current belongs to
        private static Task? _running;
        private static Process? _process;

        /// <summary>The newest known hashes (both null before the first answer).</summary>
        public static Hashes Current { get { lock (Gate) return _current; } }

        /// <summary>The 8 hex of a party identity file's "hash" line, or null.</summary>
        public static string? HashOf(string? text)
        {
            foreach (var line in (text ?? "").Split('\n'))
            {
                var l = line.Trim();
                if (l.StartsWith("hash ", StringComparison.Ordinal) && Short(l[5..]) is { } h) return h;
            }
            return null;
        }

        /// <summary>The hashes in bb-start --party-hashes' answer line ("party-hashes mods &lt;hex&gt; patches &lt;hex&gt;").</summary>
        public static Hashes? ParseAnswer(string? line)
        {
            var parts = (line ?? "").Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length != 5 || parts[0] != "party-hashes" || parts[1] != "mods" || parts[3] != "patches") return null;
            return new Hashes(Short(parts[2]), Short(parts[4]));
        }

        private static string? Short(string hex)
        {
            hex = hex.Trim();
            return hex.Length >= 8 && hex.Length <= 64 && hex.All(Uri.IsHexDigit) ? hex[..8].ToLowerInvariant() : null;
        }

        /// <summary>What decides the hashes: the start environment (mods folder, patches, FPS, ...) and the size and time of
        /// every file in the mods and patches folders and of mods.json, patches.json and bbport.ini.</summary>
        public static string Fingerprint(IReadOnlyDictionary<string, string> environment, IEnumerable<string> folders, IEnumerable<string> files)
        {
            var text = new StringBuilder();
            foreach (var (key, value) in environment.OrderBy(kv => kv.Key, StringComparer.Ordinal)) text.Append(key).Append('=').Append(value).Append('\n');
            foreach (var folder in folders.Where(Directory.Exists))
            {
                try
                {
                    foreach (var file in Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).OrderBy(f => f, StringComparer.Ordinal))
                        Stamp(text, file);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { text.Append("?").Append(folder).Append('\n'); }
            }
            foreach (var file in files) Stamp(text, file);
            return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text.ToString())));
        }

        private static void Stamp(StringBuilder text, string file)
        {
            try
            {
                var info = new FileInfo(file);
                text.Append(file).Append('|').Append(info.Exists ? info.Length : -1).Append('|')
                    .Append(info.Exists ? info.LastWriteTimeUtc.Ticks : 0).Append('\n');
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { text.Append(file).Append("|?\n"); }
        }

        /// <summary>Brings <see cref="Current"/> up to date in the background (the lobby list calls it on every refresh
        /// while it lists a game, and redraws when the answer changes; cheap while nothing changed). <paramref name="gameRunning"/>: a game of this install runs, so its own files are
        /// current and bb-start must not rewrite them.</summary>
        public static void Refresh(LauncherSettings s, bool gameRunning)
        {
            if (gameRunning)
            {
                var fromGame = new Hashes(HashOf(Read(Path.Combine(Paths.BinariesDir, "party_mods.txt"))),
                                          HashOf(Read(Path.Combine(Paths.BinariesDir, "party_patch_hash.txt"))));
                Set(fromGame, null);
                return;
            }
            if (!File.Exists(Paths.Start)) return;
            string fingerprint;
            try
            {
                fingerprint = Fingerprint(GameEnvironment.Build(s, applyMods: false),
                    new[] { GameEnvironment.ModsDir(s), GameEnvironment.PatchesDir(s) },
                    new[] { Mods.ProfilePath, Path.Combine(Paths.DataDir, "patches.json"), Paths.IniPath });
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException) { return; }
            lock (Gate)
            {
                if (_running != null || fingerprint == _fingerprint) return;
                _running = Task.Run(() => Ask(s, fingerprint));
            }
        }

        private static void Ask(LauncherSettings s, string fingerprint)
        {
            Hashes? answer = null;
            try
            {
                var info = new ProcessStartInfo(Paths.Start)
                {
                    UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8, WorkingDirectory = Paths.EngineDir,
                };
                info.ArgumentList.Add("--party-hashes");
                info.ArgumentList.Add("--game-dir");
                info.ArgumentList.Add(GameEnvironment.GameDir(s));
                foreach (var (key, value) in GameEnvironment.Build(s)) info.Environment[key] = value;
                using var process = Process.Start(info);
                if (process == null) return;
                lock (Gate) _process = process;
                process.ErrorDataReceived += (_, _) => { };
                process.BeginErrorReadLine();
                string? line;
                while ((line = process.StandardOutput.ReadLine()) != null)
                    if (ParseAnswer(line) is { } parsed) answer = parsed;
                if (!process.WaitForExit(60_000)) { try { process.Kill(true); } catch (InvalidOperationException) { } }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException or
                                          System.ComponentModel.Win32Exception) { }
            finally
            {
                lock (Gate) { _process = null; _running = null; }
            }
            // A failed answer keeps the fingerprint unset, so the next refresh asks again.
            if (answer is { } a) Set(a, fingerprint);
        }

        /// <summary>Stops a running hash check: a game start takes the engine's out folder (GameRunner.Start).</summary>
        public static void StopForGameStart()
        {
            Process? process;
            Task? running;
            lock (Gate) { process = _process; running = _running; _fingerprint = null; }
            try { process?.Kill(true); } catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { }
            try { running?.Wait(5_000); } catch (AggregateException) { }
        }

        private static void Set(Hashes value, string? fingerprint)
        {
            lock (Gate)
            {
                _current = value;
                _fingerprint = fingerprint;
            }
        }

        private static string? Read(string path)
        {
            try { return File.Exists(path) ? File.ReadAllText(path) : null; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        }
    }
}
