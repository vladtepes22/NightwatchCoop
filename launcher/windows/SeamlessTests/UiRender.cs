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
                // Each mode shows its own rows: Host and Join their party rows, single player only the saves.
                foreach (var (mode, name) in new[] { ("host", "host"), ("join", "join"), ("", "single") })
                {
                    BloodborneLauncher.Core.Workspace.Current.Settings.Set("party_mode", mode);
                    var page = new SeamlessPage();
                    page.RaiseEvent(new RoutedEventArgs(FrameworkElement.LoadedEvent));
                    var scroll = (ScrollViewer)page.Content;
                    page.Content = null;
                    var surface = new Border { Background = new SolidColorBrush(Color.FromRgb(24, 24, 28)), Child = scroll };
                    surface.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");
                    surface.Measure(new Size(920, 760));
                    surface.Arrange(new Rect(0, 0, 920, 760));
                    surface.UpdateLayout();
                    if (mode.Length > 0 && scroll.ExtentHeight <= scroll.ViewportHeight) throw new InvalidOperationException("Party settings did not lay out.");
                    foreach (var (label, fraction) in new[] { ("party", 0.0), ("rules", 0.5), ("saves", 1.0) })
                    {
                        scroll.ScrollToVerticalOffset(scroll.ScrollableHeight * fraction);
                        surface.UpdateLayout();
                        var image = new RenderTargetBitmap(920, 760, 96, 96, PixelFormats.Pbgra32);
                        image.Render(surface);
                        var encoder = new PngBitmapEncoder();
                        encoder.Frames.Add(BitmapFrame.Create(image));
                        using var output = File.Create(Path.Combine(directory, $"seamless-{name}-{label}.png"));
                        encoder.Save(output);
                    }
                }
                Console.WriteLine("PASS: seamless settings render for host, join and single player without showing a window");

                // The other settings pages build and lay out too (their first and last screen).
                foreach (var type in new[] { typeof(DisplayPage), typeof(GraphicsPage), typeof(ControlsPage), typeof(TrophiesPage),
                             typeof(SettingsPage), typeof(ModsPage), typeof(CheatsPage), typeof(EnginePage) })
                {
                    var page = (LauncherPage)Activator.CreateInstance(type)!;
                    page.RaiseEvent(new RoutedEventArgs(FrameworkElement.LoadedEvent));
                    var content = (UIElement)page.Content;
                    var scroll = FindScroll(content) ?? throw new InvalidOperationException($"{type.Name} has no scroller.");
                    page.Content = null;
                    var surface = new Border { Background = new SolidColorBrush(Color.FromRgb(24, 24, 28)), Child = content };
                    surface.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");
                    surface.Measure(new Size(920, 760));
                    surface.Arrange(new Rect(0, 0, 920, 760));
                    surface.UpdateLayout();
                    foreach (var (label, fraction) in new[] { ("top", 0.0), ("end", 1.0) })
                    {
                        scroll.ScrollToVerticalOffset(scroll.ScrollableHeight * fraction);
                        surface.UpdateLayout();
                        var image = new RenderTargetBitmap(920, 760, 96, 96, PixelFormats.Pbgra32);
                        image.Render(surface);
                        var encoder = new PngBitmapEncoder();
                        encoder.Frames.Add(BitmapFrame.Create(image));
                        using var output = File.Create(Path.Combine(directory, $"{type.Name}-{label}.png"));
                        encoder.Save(output);
                    }
                }
                Console.WriteLine("PASS: every settings page renders");

                // The Eldritch theme over the Modern layout's glass, with its art behind the Co-op page.
                Application.Current.Resources.MergedDictionaries.Add(new ResourceDictionary
                {
                    Source = new Uri("pack://application:,,,/Nightwatch Co-op;component/Themes/Glass.xaml", UriKind.Absolute),
                });
                var settings = BloodborneLauncher.Core.Workspace.Current.Settings;
                settings.Set("party_mode", "host");
                foreach (var (theme, pageType) in new[] { ("default", typeof(SeamlessPage)), ("eldritch", typeof(SeamlessPage)),
                             ("eldritch", typeof(PlayPage)), ("eldritch", typeof(TrophiesPage)) })
                {
                    settings.Set("ui_theme", theme);
                    LauncherTheme.Apply(settings, glass: true);
                    var art = new EldritchArt();
                    art.Show(LauncherTheme.Eldritch(settings));
                    var page = (LauncherPage)Activator.CreateInstance(pageType)!;
                    page.RaiseEvent(new RoutedEventArgs(FrameworkElement.LoadedEvent));
                    var content = (UIElement)page.Content;
                    page.Content = null;
                    var surface = new Grid();
                    surface.SetResourceReference(Panel.BackgroundProperty, "WindowBaseBrush");
                    surface.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");
                    surface.SetResourceReference(System.Windows.Documents.TextElement.FontFamilyProperty, "UiFont");
                    surface.Children.Add(art);
                    surface.Children.Add(new Border { Margin = new Thickness(0, 60, 0, 0), Child = content });
                    surface.Measure(new Size(1280, 800));
                    surface.Arrange(new Rect(0, 0, 1280, 800));
                    surface.UpdateLayout();
                    var image = new RenderTargetBitmap(1280, 800, 96, 96, PixelFormats.Pbgra32);
                    image.Render(surface);
                    var encoder = new PngBitmapEncoder();
                    encoder.Frames.Add(BitmapFrame.Create(image));
                    using var output = File.Create(Path.Combine(directory, $"theme-{theme}-{pageType.Name}.png"));
                    encoder.Save(output);
                    if (theme == "eldritch" && pageType == typeof(PlayPage))
                    {
                        // A surprise plays on the stage in front of the panes.
                        var stage = new Canvas();
                        surface.Children.Add(stage);
                        art.Stage = stage;
                        surface.UpdateLayout();
                        typeof(EldritchArt).GetMethod("Surprise", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!
                            .Invoke(art, null);

                    }
                    art.Show(false);
                }
                settings.Set("ui_theme", "default");
                Console.WriteLine("PASS: the Eldritch theme renders with its art");
            }
            catch (Exception error) { failure = error; }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (failure != null) throw new InvalidOperationException("Launcher rendering failed", failure);
    }

    private static ScrollViewer? FindScroll(object at) => at as ScrollViewer ??
        (at is DependencyObject parent ? LogicalTreeHelper.GetChildren(parent).Cast<object>().Select(FindScroll).FirstOrDefault(s => s != null) : null);
}
