using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using BloodborneLauncher.Views;

internal static class UiRender
{
    public static void Check(string directory)
    {
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try
            {
                Directory.CreateDirectory(directory);
                var app = new BloodborneLauncher.App();
                app.InitializeComponent();
                foreach (var classic in new[] { false, true })
                {
                    SettingRow.Classic = classic;
                    var page = new SeamlessPage();
                    page.RaiseEvent(new RoutedEventArgs(FrameworkElement.LoadedEvent));
                    var scroll = (ScrollViewer)page.Content;
                    page.Content = null;
                    var surface = new Border { Background = new SolidColorBrush(Color.FromRgb(24, 24, 28)), Child = scroll };
                    surface.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");
                    surface.Measure(new Size(920, 760));
                    surface.Arrange(new Rect(0, 0, 920, 760));
                    surface.UpdateLayout();
                    if (scroll.ExtentHeight <= scroll.ViewportHeight) throw new InvalidOperationException("Party settings did not lay out.");
                    foreach (var (label, fraction) in new[] { ("party", 0.0), ("rules", 0.5), ("saves", 1.0) })
                    {
                        scroll.ScrollToVerticalOffset(scroll.ScrollableHeight * fraction);
                        surface.UpdateLayout();
                        var image = new RenderTargetBitmap(920, 760, 96, 96, PixelFormats.Pbgra32);
                        image.Render(surface);
                        var encoder = new PngBitmapEncoder();
                        encoder.Frames.Add(BitmapFrame.Create(image));
                        using var output = File.Create(Path.Combine(directory, $"seamless-{(classic ? "classic" : "compact")}-{label}.png"));
                        encoder.Save(output);
                    }
                }
                Console.WriteLine("PASS: seamless settings render in compact and classic layouts without showing a window");
            }
            catch (Exception error) { failure = error; }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (failure != null) throw new InvalidOperationException("Launcher rendering failed", failure);
    }
}
