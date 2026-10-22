using System.Diagnostics;
using System.IO;
#if !AVALONIA
using System.Windows;
using System.Windows.Controls;
#endif

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Nightwatch Co-op v0.2.0 beta 8 renames the launcher from "Lantern Engine.exe" to
    /// "Nightwatch Co-op.exe" (Lantern Engine itself renamed it from "Bloodborne Launcher.exe" in
    /// 0.9.34 beta 8). Packages carry a small "Lantern Engine.exe" forwarder (launcher\LanternForward):
    /// launchers before the rename update by that name and restart it, and it opens Nightwatch Co-op.
    /// Once here: the launcher copied under its new name (when an old launcher updated only itself),
    /// shortcuts on the desktop, in the Start menu and on the taskbar pointed at the new name, and
    /// the old files deleted, so the folder holds only Nightwatch Co-op.exe and Bloodborne.exe. A
    /// window says so while it works; the forwarder an update brings back is deleted quietly.
    /// </summary>
    public static class LanternRename
    {
        private const long ForwarderMaxBytes = 1_000_000; // the forwarder is ~170 KB, the launcher ~75 MB

        /// <summary>Returns true when this process should end now (it started the renamed launcher).</summary>
        public static bool Run(string[] args)
        {
#if AVALONIA
            // Only Windows packages ever had the old name.
            if (!OperatingSystem.IsWindows()) return false;
#endif
            try
            {
                var self = Environment.ProcessPath;
                if (self == null) return false;
                var folder = Path.GetDirectoryName(self)!;
                var renamed = Path.Combine(folder, Updater.ExeName);
                var runningOld = Updater.OldExeNames.Any(name =>
                    string.Equals(Path.GetFileName(self), name, StringComparison.OrdinalIgnoreCase));
                if (runningOld)
                {
                    // The full launcher under an old name (an old launcher updated only itself): it
                    // becomes Nightwatch Co-op.exe, which finishes the job (this file is in use meanwhile).
                    Show(new() { ($"Renaming the launcher to {Updater.ExeName}", () =>
                    {
                        if (!File.Exists(renamed) || new FileInfo(renamed).Length < ForwarderMaxBytes ||
                            File.GetLastWriteTimeUtc(renamed) < File.GetLastWriteTimeUtc(self))
                            Retry(() => File.Copy(self, renamed, overwrite: true));
                    }) });
                    if (!File.Exists(renamed)) return false;
                    Process.Start(new ProcessStartInfo(renamed, args) { UseShellExecute = false, WorkingDirectory = folder });
                    return true;
                }
                var steps = new List<(string Text, Action Work)>();
                var quiet = new List<Action>();
                var show = false;
                foreach (var name in Updater.OldExeNames)
                {
                    var old = Path.Combine(folder, name);
                    if (!File.Exists(old)) continue;
                    var shortcuts = Shortcuts.Pointing(old);
                    if (shortcuts.Count > 0)
                        steps.Add(($"Pointing {shortcuts.Count} shortcut(s) at {Updater.ExeName}", () => Shortcuts.Retarget(shortcuts, renamed)));
                    // The old launcher or the forwarder that started this one may still be closing.
                    Action delete = () => Retry(() => File.Delete(old), attempts: 40);
                    steps.Add(($"Removing {name}", delete));
                    quiet.Add(delete);
                    show |= new FileInfo(old).Length > ForwarderMaxBytes || shortcuts.Count > 0;
                }
                if (show)
                    Show(steps);
                else if (quiet.Count > 0)
                    _ = Task.Run(() => { foreach (var delete in quiet) try { delete(); } catch (Exception) { } });
            }
            catch (Exception)
            {
                // Never stop the launcher from starting.
            }
            return false;
        }

        private static void Retry(Action action, int attempts = 10)
        {
            for (var attempt = 1; ; attempt++)
            {
                try { action(); return; }
                catch (Exception e) when (attempt < attempts && e is IOException or UnauthorizedAccessException)
                {
                    Thread.Sleep(250);
                }
            }
        }

        private static void Show(List<(string Text, Action Work)> steps)
        {
#if AVALONIA
            foreach (var (_, work) in steps)
            {
                try { work(); } catch (Exception) { /* the next step still runs */ }
            }
            return;
#else
            if (steps.Count == 0) return;
            var status = new TextBlock { Margin = new Thickness(0, 10, 0, 0), Opacity = 0.8 };
            var bar = new ProgressBar { Height = 6, Minimum = 0, Maximum = steps.Count, Margin = new Thickness(0, 12, 0, 0) };
            var window = new Window
            {
                Title = "Nightwatch Co-op",
                Width = 440,
                SizeToContent = SizeToContent.Height,
                ResizeMode = ResizeMode.NoResize,
                WindowStartupLocation = WindowStartupLocation.CenterScreen,
                WindowStyle = WindowStyle.ToolWindow,
                Topmost = true,
                Content = new StackPanel
                {
                    Margin = new Thickness(22),
                    Children =
                    {
                        new TextBlock { Text = "Updating to Nightwatch Co-op", FontSize = 16, FontWeight = FontWeights.SemiBold },
                        new TextBlock
                        {
                            Text = "The launcher has a new name. Renaming files and fixing shortcuts; your game, saves, " +
                                   "settings and mods stay as they are. Don't close this window: it takes a few seconds.",
                            TextWrapping = TextWrapping.Wrap,
                            Margin = new Thickness(0, 8, 0, 0),
                        },
                        status,
                        bar,
                    },
                },
            };
            window.Closing += (_, e) => e.Cancel = bar.Value < steps.Count; // only the work closes it
            window.Loaded += async (_, _) =>
            {
                for (var i = 0; i < steps.Count; ++i)
                {
                    status.Text = steps[i].Text + "…";
                    var work = steps[i].Work;
                    try { await Task.Run(work); } catch (Exception) { /* the next step still runs */ }
                    bar.Value = i + 1;
                }
                status.Text = "Done.";
                await Task.Delay(400);
                window.Close();
            };
            var mode = Application.Current.ShutdownMode;
            Application.Current.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            window.ShowDialog();
            Application.Current.ShutdownMode = mode;
#endif
        }

        /// <summary>Shortcuts (.lnk) on the desktop, in the Start menu and pinned to the taskbar.</summary>
#if AVALONIA
        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
#endif
        private static class Shortcuts
        {
            private static IEnumerable<string> Folders()
            {
                yield return Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                yield return Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory);
                yield return Environment.GetFolderPath(Environment.SpecialFolder.Programs);
                yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "Microsoft", "Internet Explorer", "Quick Launch", "User Pinned", "TaskBar");
            }

            public static List<string> Pointing(string target)
            {
                var found = new List<string>();
                var shell = Shell();
                if (shell == null) return found;
                foreach (var folder in Folders().Where(Directory.Exists))
                {
                    IEnumerable<string> links;
                    try { links = Directory.EnumerateFiles(folder, "*.lnk", SearchOption.AllDirectories).ToList(); }
                    catch (Exception) { continue; }
                    foreach (var link in links)
                    {
                        try
                        {
                            var path = (string)shell.CreateShortcut(link).TargetPath;
                            if (string.Equals(Path.GetFullPath(path), Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase))
                                found.Add(link);
                        }
                        catch (Exception) { }
                    }
                }
                return found;
            }

            public static void Retarget(List<string> links, string target)
            {
                var shell = Shell();
                if (shell == null) return;
                foreach (var link in links)
                {
                    try
                    {
                        var shortcut = shell.CreateShortcut(link);
                        shortcut.TargetPath = target;
                        shortcut.WorkingDirectory = Path.GetDirectoryName(target);
                        shortcut.IconLocation = target + ",0";
                        shortcut.Save();
                    }
                    catch (Exception) { }
                }
            }

            private static dynamic? Shell() =>
                Type.GetTypeFromProgID("WScript.Shell") is { } type ? Activator.CreateInstance(type) : null;
        }
    }
}
