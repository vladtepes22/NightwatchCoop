using System.IO;
using BloodborneLauncher.Build;

namespace BloodborneLauncher.Core
{
    /// <summary>What happened to the old settings folder at this start (Paths.ResolveConfig).</summary>
    public enum ConfigMove { None, OldKept, Failed }

    /// <summary>
    /// Where things live. The package folder (PortDir) holds the launcher, Bloodborne.exe and the
    /// player's files - bbport.ini, mods.json, patches.json, user, mods, logs (its data folder,
    /// BB_DATA_DIR) - and the engine in engine\: out\bb-probe.exe, scripts\run_windows.py and a
    /// bundled Python. Older packages had the engine at the top (EngineDirOf handles both).
    /// Launcher settings are per-user, in %APPDATA%\nightwatch\settings.json.
    /// </summary>
    public static class Paths
    {
        /// <summary>The launcher's settings: %APPDATA%\nightwatch, or BB_LAUNCHER_CONFIG_DIR (a second launcher on one PC,
        /// e.g. a local co-op test, keeps its own and never moves anything).</summary>
        private static readonly (string Dir, ConfigMove Move) Config =
            Environment.GetEnvironmentVariable("BB_LAUNCHER_CONFIG_DIR") is { Length: > 0 } configDir
                ? (configDir, ConfigMove.None)
                : ResolveConfig(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), OtherLaunchersRunning,
                    System.Reflection.Assembly.GetEntryAssembly() == typeof(Paths).Assembly);

        public static readonly string ConfigDir = Config.Dir;
        public static readonly ConfigMove ConfigMoved = Config.Move;

        public static readonly string ConfigFile = Path.Combine(ConfigDir, "settings.json");

        /// <summary>Picks the settings folder before anything reads it, moving the old default on first use. The new one
        /// wins when both exist, and neither is touched then. While another launcher runs, or when the rename fails, the
        /// old folder stays in use for this run: its contents are never split, merged or started fresh. Only the launcher
        /// itself moves it (<paramref name="mayMove"/>), never a test program that links this class.</summary>
        public static (string Dir, ConfigMove Move) ResolveConfig(string appData, Func<bool> othersRunning, bool mayMove)
        {
            string old = Path.Combine(appData, "bloodborne-seamless"), now = Path.Combine(appData, "nightwatch");
            using var turn = new Mutex(false, @"Local\NightwatchConfigMove");
            var held = false;
            try
            {
                try { held = turn.WaitOne(TimeSpan.FromSeconds(5)); }
                catch (AbandonedMutexException) { held = true; }
                if (Directory.Exists(now)) return (now, Directory.Exists(old) ? ConfigMove.OldKept : ConfigMove.None);
                if (!Directory.Exists(old)) return (now, ConfigMove.None);
                if (!held || !mayMove) return (old, ConfigMove.Failed);
                // An update's restart: the launcher it replaces is on its way out.
                for (var i = 0; i < 15 && othersRunning(); i++) Thread.Sleep(200);
                if (othersRunning()) return (old, ConfigMove.Failed);
                try
                {
                    Directory.Move(old, now);
                    return (now, ConfigMove.None);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return (old, ConfigMove.Failed); }
            }
            finally { if (held) turn.ReleaseMutex(); }
        }

        private static bool OtherLaunchersRunning()
        {
            using var self = System.Diagnostics.Process.GetCurrentProcess();
            var same = System.Diagnostics.Process.GetProcessesByName(self.ProcessName);
            var others = same.Any(p => p.Id != self.Id);
            foreach (var p in same) p.Dispose();
            return others;
        }

        /// <summary>The port folder in use: the "port_dir" setting, else one found by ResolvePort.</summary>
        public static string PortDir { get; set; } = "";

        public static string DataDir =>
            Environment.GetEnvironmentVariable("BB_DATA_DIR") is { Length: > 0 } dir ? dir : PortDir;

        public static string IniPath =>
            Environment.GetEnvironmentVariable("BB_CONFIG") is { Length: > 0 } path
                ? path
                : Path.Combine(DataDir, "bbport.ini");

        /// <summary>An engine program's file name: name.exe, or name alone in the Linux package
        /// (the Avalonia launcher there).</summary>
        public static string Program(string name) =>
#if AVALONIA
            OperatingSystem.IsWindows() ? name + ".exe" : name;
#else
            name + ".exe";
#endif

        /// <summary>A package's engine folder: its engine\, or the package itself (older layout).</summary>
        public static string EngineDirOf(string dir) =>
            File.Exists(Path.Combine(dir, "engine", "out", Program("bb-probe"))) ||
            File.Exists(Path.Combine(dir, "engine", "scripts", "run_windows.py"))
                ? Path.Combine(dir, "engine")
                : dir;

        /// <summary>The engine in use: out\bb-probe.exe, scripts, patches, shaders and Python.</summary>
        public static string EngineDir => EngineDirOf(PortDir);

        /// <summary>The engine and its DLLs.</summary>
        public static string BinariesDir => Path.Combine(EngineDir, "out");

        /// <summary>NVIDIA's DLSS runtime beside the engine (nvngx_dlss.dll; on Linux
        /// libnvidia-ngx-dlss.so.&lt;version&gt;), or null when the package has none.</summary>
        public static string? DlssRuntime
        {
            get
            {
                if (OperatingSystem.IsWindows())
                {
                    var dll = Path.Combine(BinariesDir, "nvngx_dlss.dll");
                    return File.Exists(dll) ? dll : null;
                }
                try
                {
                    return Directory.Exists(BinariesDir)
                        ? Directory.GetFiles(BinariesDir, "libnvidia-ngx-dlss.so.*").OrderBy(f => f, StringComparer.Ordinal).LastOrDefault()
                        : null;
                }
                catch (IOException)
                {
                    return null;
                }
            }
        }

        /// <summary>The DLSS runtime's version: the DLL's file version, or the end of the .so's name.</summary>
        public static string DlssVersion(string runtime) => OperatingSystem.IsWindows()
            ? System.Diagnostics.FileVersionInfo.GetVersionInfo(runtime).FileVersion ?? ""
            : Path.GetFileName(runtime)["libnvidia-ngx-dlss.so.".Length..];

        /// <summary>The Bloodborne PC engine: out\bb-probe.exe and what starts it (out\bb-start.exe, or
        /// scripts\run_windows.py before 0.9.32), in the folder or its engine\.</summary>
        public static bool IsPortDir(string? dir)
        {
            if (string.IsNullOrWhiteSpace(dir)) return false;
            var engine = EngineDirOf(dir);
            return File.Exists(Path.Combine(engine, "out", Program("bb-probe"))) &&
                   (File.Exists(Path.Combine(engine, "out", Program("bb-start"))) ||
                    File.Exists(Path.Combine(engine, "scripts", "run_windows.py")));
        }

        /// <summary>The native start (0.9.32 and later): the boot image, mods, patches, then the game.</summary>
        public static string Start => Path.Combine(EngineDir, "out", Program("bb-start"));

        /// <summary>The Python an older engine started with (scripts\run_windows.py).</summary>
        public static string Python => Path.Combine(EngineDir, "tools", "python", "python.exe");

        /// <summary>The party save tools (party_save_tool.py and its modules) and their own Python, in the
        /// package's seamless\ folder: the engine's scripts\ and tools\ are removed once it starts natively
        /// (LayoutCleanup). A source checkout runs them from engine\scripts with the engine's Python.</summary>
        public static string SeamlessScripts =>
            File.Exists(Path.Combine(PortDir, "seamless", "scripts", "party_save_tool.py"))
                ? Path.Combine(PortDir, "seamless", "scripts")
                : Path.Combine(EngineDir, "scripts");

        public static string SeamlessPython =>
            File.Exists(Path.Combine(PortDir, "seamless", "python", "python.exe"))
                ? Path.Combine(PortDir, "seamless", "python", "python.exe")
                : File.Exists(Python) ? Python : "py";

        /// <summary>The co-op connection doctor (net_doctor.bat): seamless\ in a package, the engine's folder in a source checkout.</summary>
        public static string NetDoctorBat =>
            File.Exists(Path.Combine(PortDir, "seamless", "net_doctor.bat"))
                ? Path.Combine(PortDir, "seamless", "net_doctor.bat")
                : Path.Combine(EngineDir, "net_doctor.bat");

        /// <summary>The port next to the game folder (its usual layout: &lt;port&gt;\CUSA03173).</summary>
        public static string? PortBesideGame(string gameDir)
        {
            try
            {
                var parent = string.IsNullOrWhiteSpace(gameDir) ? null : Directory.GetParent(gameDir)?.FullName;
                return IsPortDir(parent) ? parent : null;
            }
            catch
            {
                return null;
            }
        }

        /// <summary>A port among the folders beside the game folder (E:\games\CUSA03173 next to
        /// E:\games\bbport-windows), the one with the newest bb-probe.exe.</summary>
        public static string? PortNearGame(string gameDir)
        {
            try
            {
                var parent = string.IsNullOrWhiteSpace(gameDir) ? null : Directory.GetParent(gameDir)?.FullName;
                if (parent == null) return null;
                return Directory.GetDirectories(parent)
                    .Where(IsPortDir)
                    .OrderByDescending(dir => new[] { "bin", "out" }
                        .Select(b => new FileInfo(Path.Combine(EngineDirOf(dir), b, Program("bb-probe"))))
                        .Where(f => f.Exists).Select(f => f.LastWriteTimeUtc).DefaultIfEmpty(DateTime.MinValue).Max())
                    .FirstOrDefault();
            }
            catch
            {
                return null;
            }
        }

        /// <summary>A port folder above this exe (the launcher copied into the port).</summary>
        public static string? DetectPortDir()
        {
            foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
                for (var dir = new DirectoryInfo(start); dir != null; dir = dir.Parent)
                    if (IsPortDir(dir.FullName))
                        return dir.FullName.TrimEnd(Path.DirectorySeparatorChar);
            return null;
        }

        /// <summary>launcher/windows in the source checkout, when this copy was built from it.</summary>
        public static string? SourceRoot => SourceStamp.LocateSources(AppContext.BaseDirectory);

        public static string Expand(string path) =>
            Environment.ExpandEnvironmentVariables(path.StartsWith("~")
                ? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) + path[1..]
                : path);

        public static void OpenFolder(string path)
        {
            try
            {
                Directory.CreateDirectory(path);
#if AVALONIA
                if (!OperatingSystem.IsWindows())
                {
                    // The desktop's file manager.
                    var open = new System.Diagnostics.ProcessStartInfo("xdg-open") { UseShellExecute = false };
                    open.ArgumentList.Add(path);
                    System.Diagnostics.Process.Start(open)?.Dispose();
                    return;
                }
#endif
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", $"\"{path}\"")
                    { UseShellExecute = true });
            }
            catch
            {
                // Nothing useful to do if Explorer will not open.
            }
        }
    }
}
