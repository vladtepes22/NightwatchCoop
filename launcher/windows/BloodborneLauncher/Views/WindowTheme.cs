using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace BloodborneLauncher.Views;

/// <summary>Colours a plain window's frame like the launcher: a dark title bar, and on Windows 11 the caption, text and
/// border taken from the window's own theme resources, so the white system frame does not show around a dark window.
/// Windows 10 only gets the dark title bar; a failed call leaves the frame as it was.</summary>
internal static class WindowTheme
{
    private const int ImmersiveDarkMode = 20, BorderColor = 34, CaptionColor = 35, TextColor = 36;

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

    internal static void Apply(Window window, string captionBrush = "DialogBackgroundBrush",
        string textBrush = "TextFillColorPrimaryBrush", string borderBrush = "CardStrokeColorDefaultBrush")
    {
        var handle = new WindowInteropHelper(window).Handle;
        if (handle == IntPtr.Zero) return;
        var dark = 1;
        DwmSetWindowAttribute(handle, ImmersiveDarkMode, ref dark, sizeof(int));
        Set(window, handle, CaptionColor, captionBrush, null);
        Set(window, handle, TextColor, textBrush, null);
        Set(window, handle, BorderColor, borderBrush, captionBrush);
    }

    // A COLORREF is 0x00BBGGRR and has no alpha: a translucent brush (the card stroke) is mixed over the opaque one.
    private static void Set(Window window, IntPtr handle, int attribute, string brush, string? opaqueOver)
    {
        if (window.TryFindResource(brush) is not SolidColorBrush solid) return;
        var colour = solid.Color;
        if (colour.A < 255 && opaqueOver != null && window.TryFindResource(opaqueOver) is SolidColorBrush under)
        {
            var a = colour.A / 255.0;
            colour = Color.FromRgb((byte)(colour.R * a + under.Color.R * (1 - a)), (byte)(colour.G * a + under.Color.G * (1 - a)),
                (byte)(colour.B * a + under.Color.B * (1 - a)));
        }
        var value = colour.R | colour.G << 8 | colour.B << 16;
        DwmSetWindowAttribute(handle, attribute, ref value, sizeof(int));
    }
}
