using System.Diagnostics;
using System.IO;
using System.Text;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Faster loading with converted game files (visy 0.8.1, zstd081): bb-zstdpack copies the game's zlib-packed
    /// DCX files once, recompressed with zstd, into a folder next to the game folder (&lt;game folder&gt;-zstd); the
    /// engine (BB_ZSTD_DIR, gpu/shim/bbport_zstd_files.cpp) reads a listed file from there while the original is
    /// unchanged, and decodes it about twice as fast. The game folder is never written, so every party check reads
    /// the same files. Off by default; turning it off or deleting the folder goes back.
    /// </summary>
    public static class ZstdFiles
    {
        public static string Packer => Path.Combine(Paths.BinariesDir, Paths.Program("bb-zstdpack"));

        public static bool Supported => File.Exists(Packer);

        /// <summary>The converted folder for this game folder ("" without a game folder).</summary>
        public static string Dir(LauncherSettings s)
        {
            var game = GameEnvironment.GameDir(s);
            if (game.Length == 0) return "";
            var full = Path.GetFullPath(game).TrimEnd('\\', '/');
            return full + "-zstd";
        }

        /// <summary>(converted files, finished, bytes) from the folder's bbzstd.manifest; (0, false, 0) without one.</summary>
        public static (int Count, bool Done, long Bytes) Summary(string dir)
        {
            int count = 0;
            bool done = false;
            long bytes = 0;
            try
            {
                if (dir.Length == 0) return (0, false, 0);
                foreach (var line in File.ReadLines(Path.Combine(dir, "bbzstd.manifest")))
                {
                    if (line.StartsWith("F\t", StringComparison.Ordinal))
                    {
                        count++;
                        var parts = line.Split('\t');
                        if (parts.Length > 4 && long.TryParse(parts[4], out var size)) bytes += size;
                    }
                    else if (line.StartsWith("DONE", StringComparison.Ordinal)) done = true;
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return (0, false, 0); }
            return (count, done, bytes);
        }

        /// <summary>The engine reads converted files only with the option on and a conversion there (the engine
        /// itself still uses only the files the manifest lists whose original is unchanged).</summary>
        public static void AddEnvironment(LauncherSettings s, IDictionary<string, string> env)
        {
            if (!s.Bool("zstd_files") || s.Bool("safe_mode_next")) return;
            var dir = Dir(s);
            if (Summary(dir).Count > 0) env["BB_ZSTD_DIR"] = dir;
        }

        private static Process? _running;

        public static bool Busy => _running is { HasExited: false };

        /// <summary>convert, then verify. Progress lines go to <paramref name="progress"/> (any thread). Returns the
        /// exit code (0 done and verified, 3 not enough free space) and the last line the tool wrote.</summary>
        public static async Task<(int Code, string Tail)> Convert(string game, string dir, Action<string> progress)
        {
            var (code, tail) = await Run("convert", game, dir, progress);
            if (code == 0) (code, tail) = await Run("verify", game, dir, progress);
            return (code, tail);
        }

        public static void Stop()
        {
            try { _running?.Kill(entireProcessTree: true); } catch { /* already gone */ }
        }

        private static async Task<(int, string)> Run(string verb, string game, string dir, Action<string> progress)
        {
            var info = new ProcessStartInfo(Packer)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            foreach (var a in new[] { verb, "--game", game, "--out", dir }) info.ArgumentList.Add(a);
            using var p = Process.Start(info) ?? throw new IOException("bb-zstdpack did not start");
            _running = p;
            var tail = "";
            // Progress lines end with a carriage return, not a newline: split on both.
            async Task Pump(StreamReader r)
            {
                var buf = new char[4096];
                var line = new StringBuilder();
                int n;
                while ((n = await r.ReadAsync(buf, 0, buf.Length)) > 0)
                    for (int i = 0; i < n; i++)
                    {
                        if (buf[i] is '\r' or '\n')
                        {
                            var t = line.ToString().Trim();
                            line.Clear();
                            if (t.Length == 0) continue;
                            tail = t;
                            progress(t);
                        }
                        else line.Append(buf[i]);
                    }
                if (line.ToString().Trim() is { Length: > 0 } last) { tail = last; progress(last); }
            }
            await Task.WhenAll(Pump(p.StandardOutput), Pump(p.StandardError));
            await p.WaitForExitAsync();
            _running = null;
            return (p.ExitCode, tail);
        }
    }
}
