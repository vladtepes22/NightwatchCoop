using System.Globalization;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Media;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views;

/// <summary>The launcher's size (settings.json ui_scale: "auto" or a percent). Windows scales the launcher with the
/// display's own scaling; Wine (Linux, Steam Deck) usually reports 96 DPI on any screen, so on a 1440p or 4K screen the
/// launcher came out tiny. A window's content is scaled with a layout transform (its pop-ups, drop-downs and tooltips
/// follow it), and a window of fixed size grows by as much, kept on the screen.</summary>
internal static class UiScale
{
    public static Choice[] Choices => new[]
    {
        new Choice(L.T("Automatic"), "auto", L.T("Matches the screen on Linux; 100% on Windows, which scales the launcher itself")),
        new Choice("100%", "100"), new Choice("125%", "125"), new Choice("150%", "150"),
        new Choice("175%", "175"), new Choice("200%", "200"), new Choice("250%", "250"),
    };

    /// <summary>The size the settings ask for, as a factor (1 = 100%).</summary>
    public static double Factor(LauncherSettings settings)
    {
        var value = settings.Str("ui_scale");
        double factor;
        if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var percent))
            factor = Math.Clamp(percent / 100, 1, 3);
        else
            // Automatic: Wine's screen in 96-DPI units over a 1080p one, in steps of 25%. A Wine prefix set to a higher DPI
            // (winecfg) already scales the launcher, its screen reads smaller and this stays at 100%.
            factor = WineHost.IsWine ? Math.Clamp(Math.Floor(SystemParameters.PrimaryScreenHeight / 1080 * 4) / 4, 1, 2) : 1;
        // Never larger than lets the main window's smallest layout (820 x 560) fit the screen: past that the title bar
        // and tabs overlap.
        var work = SystemParameters.WorkArea;
        return Math.Max(1, Math.Min(factor, Math.Min((work.Width - 16) / 820, (work.Height - 16) / 560)));
    }

    private sealed class State
    {
        public required bool Resize;
        public required object TextMode;  // the window's own TextFormattingMode (local value or UnsetValue)
        public double Applied = 1;
    }

    private static readonly ConditionalWeakTable<Window, State> Windows = new();

    /// <summary>Called at the end of a window's constructor, after its content and size are set. <paramref name="resize"/>:
    /// the window has a size of its own (not SizeToContent, not a layer over its owner), which grows with the content.</summary>
    public static void Attach(Window window, bool resize)
    {
        var state = new State { Resize = resize, TextMode = window.ReadLocalValue(TextOptions.TextFormattingModeProperty) };
        Windows.AddOrUpdate(window, state);
        Apply(window, state, Factor(Workspace.Current.Settings));
    }

    /// <summary>The setting changed: every open window takes the new size now.</summary>
    public static void Changed()
    {
        var factor = Factor(Workspace.Current.Settings);
        foreach (Window window in Application.Current.Windows)
            if (Windows.TryGetValue(window, out var state))
                Apply(window, state, factor);
    }

    private static void Apply(Window window, State state, double factor)
    {
        if (window.Content is FrameworkElement content)
            content.LayoutTransform = factor == 1 ? Transform.Identity : new ScaleTransform(factor, factor);
        // Display-mode text is laid out on the unscaled pixel grid and comes out blurred when scaled.
        if (factor != 1) window.SetValue(TextOptions.TextFormattingModeProperty, TextFormattingMode.Ideal);
        else if (state.TextMode == DependencyProperty.UnsetValue) window.ClearValue(TextOptions.TextFormattingModeProperty);
        else window.SetValue(TextOptions.TextFormattingModeProperty, state.TextMode);

        var ratio = factor / state.Applied;
        state.Applied = factor;
        if (window is MainWindow main) main.ScaleTitleBar(factor);
        if (!state.Resize || ratio == 1) return;
        var work = SystemParameters.WorkArea;
        window.MinWidth = Math.Min(window.MinWidth * ratio, work.Width - 16);
        window.MinHeight = Math.Min(window.MinHeight * ratio, work.Height - 16);
        if (window.WindowState != WindowState.Normal) return;
        double width = window.IsLoaded ? window.ActualWidth : window.Width, height = window.IsLoaded ? window.ActualHeight : window.Height;
        if (double.IsNaN(width) || double.IsNaN(height)) return;
        var newWidth = Math.Max(window.MinWidth, Math.Min(width * ratio, work.Width - 16));
        var newHeight = Math.Max(window.MinHeight, Math.Min(height * ratio, work.Height - 16));
        if (window.IsLoaded)
        {
            // An open window keeps its centre, and stays on the work area.
            window.Left = Math.Max(work.Left, Math.Min(window.Left - (newWidth - width) / 2, work.Right - newWidth));
            window.Top = Math.Max(work.Top, Math.Min(window.Top - (newHeight - height) / 2, work.Bottom - newHeight));
        }
        window.Width = newWidth;
        window.Height = newHeight;
    }
}
