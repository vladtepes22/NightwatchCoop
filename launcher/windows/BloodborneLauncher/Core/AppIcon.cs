using System.IO;
#if AVALONIA
using Window = Avalonia.Controls.Window;
#else
using System.Windows;
using System.Windows.Media.Imaging;
#endif

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Settings › Appearance › App icon (app_icon): the launcher window's and the game window's
    /// icon: the build's own (app.ico) whatever is chosen; the setting is kept for older settings files.
    /// </summary>
    public static class AppIcon
    {
        public static readonly Choice[] Choices =
        {
            new(L.T("Auto"), "auto"),
        };

        private static string Resource(string choice) => "app.ico";

        /// <summary>The chosen icon's .ico file, as the launcher carries it.</summary>
        public static byte[]? Bytes(string choice)
        {
            try
            {
#if AVALONIA
                using var source = typeof(AppIcon).Assembly.GetManifestResourceStream(Resource(choice));
#else
                using var source = Application.GetResourceStream(new Uri($"pack://application:,,,/{Resource(choice)}"))?.Stream;
#endif
                if (source == null) return null;
                using var memory = new MemoryStream();
                source.CopyTo(memory);
                return memory.ToArray();
            }
            catch (IOException)
            {
                return null;
            }
        }

        /// <summary>The launcher window shows the chosen icon; the game window takes it at its next start.</summary>
        public static void Apply(Window? window, string choice)
        {
            if (window == null) return;
            try
            {
#if AVALONIA
                if (Bytes(choice) is { } bytes)
                    window.Icon = new Avalonia.Controls.WindowIcon(new MemoryStream(bytes));
#else
                window.Icon = BitmapFrame.Create(new Uri($"pack://application:,,,/{Resource(choice)}"));
#endif
            }
            catch (Exception)
            {
                // The window keeps the icon it has.
            }
        }

        /// <summary>The game window's icon file (engine\out\bloodborne.ico), when it differs.</summary>
        public static void SyncGameWindow(string package, string choice)
        {
            try
            {
                var target = Path.Combine(Paths.EngineDirOf(package), "out", "bloodborne.ico");
                if (!Directory.Exists(Path.GetDirectoryName(target))) return;
                if (Bytes(choice) is not { } bytes) return;
                if (!File.Exists(target) || !File.ReadAllBytes(target).AsSpan().SequenceEqual(bytes))
                    File.WriteAllBytes(target, bytes);
            }
            catch (Exception)
            {
                // The old icon stays: nothing else depends on it.
            }
        }
    }
}
