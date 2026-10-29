using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using BloodborneLauncher.Core;
using BloodborneLauncher.Views;
using Wpf.Ui.Markup;
using Wpf.Ui.Appearance;
using Button = Wpf.Ui.Controls.Button;
using TextBox = Wpf.Ui.Controls.TextBox;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length != 1) { Console.Error.WriteLine("Usage: SaveEditorShots <output-folder>"); return 2; }
        try
        {
            Directory.CreateDirectory(args[0]);
            // Plain Application, never the launcher's App/Workspace startup path. No HWND is shown.
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            app.Resources.MergedDictionaries.Add(new ThemesDictionary { Theme = ApplicationTheme.Dark });
            app.Resources.MergedDictionaries.Add(new ControlsDictionary());
            foreach (var file in new[] { "Charcoal", "Styles", "ScrollBar" })
                app.Resources.MergedDictionaries.Add(new ResourceDictionary { Source = new Uri($"pack://application:,,,/Nightwatch Co-op;component/Themes/{file}.xaml") });
            int shots = 0;
            foreach (var theme in new[] { "default", "crimson", "moon" })
            {
                var settings = new LauncherSettings(); settings.Set("ui_theme", theme); LauncherTheme.Apply(settings, false);
                foreach (var size in new[] { (900, 700), (1200, 800) })
                foreach (var state in new[] { "character", "inventory", "backups", "empty", "running" })
                {
                    var source = new FakeSource(state == "empty", state == "running");
                    var window = new SaveEditorWindow(source);
                    window.LoadAsync().GetAwaiter().GetResult();
                    window.SelectTab(state == "inventory" ? 1 : state is "backups" or "empty" ? 2 : 0);
                    var root = Mount(window, size.Item1, size.Item2);
                    Check(root, source);
                    Render(root, size.Item1, size.Item2, 1, Path.Combine(args[0], $"{theme}-{size.Item1}x{size.Item2}-{state}.png"));
                    shots++; window.Close();
                }
            }
            foreach (var scale in new[] { 1.25, 1.5 })
            foreach (var tab in new[] { 0, 1, 2 })
            {
                var window = new SaveEditorWindow(new FakeSource(false, false)); window.LoadAsync().GetAwaiter().GetResult();
                window.SelectTab(tab);
                var root = Mount(window, 700, 700);
                string state = tab == 0 ? "" : tab == 1 ? "inventory-" : "backups-";
                Render(root, 700, 700, scale, Path.Combine(args[0], $"moon-700x700-{state}dpi{scale * 100:0}.png")); shots++; window.Close();
            }
            {
                var window = new SaveEditorWindow(new FakeSource(false, false)); window.LoadAsync().GetAwaiter().GetResult();
                var root = Mount(window, 1400, 800);
                var hood = Descendants(root).OfType<System.Windows.Shapes.Path>().First(p => p.Data?.Bounds.Height > 50);
                var before = hood.Stroke;
                var settings = new LauncherSettings(); settings.Set("ui_theme", "crimson"); LauncherTheme.Apply(settings, false);
                System.Windows.Threading.Dispatcher.CurrentDispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                root.UpdateLayout();
                Require(!Equals(before, hood.Stroke), "Emblem did not follow live theme change");
                Render(root, 1400, 800, 1, Path.Combine(args[0], "crimson-1400x800-live-theme.png")); shots++; window.Close();
            }
            Console.WriteLine($"PASS: {shots} off-screen PNGs; fixtures, dirty state, validation, filtering and write protection checked.");
            app.Shutdown(); return 0;
        }
        catch (Exception e) { Console.Error.WriteLine(e); return 1; }
    }
    private static Border Mount(SaveEditorWindow window, int width, int height)
    {
        var content = (FrameworkElement)window.Content; window.Content = null;
        var root = new Border { Child = content, Width = width, Height = height };
        window.Content = root;
        root.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
        Layout(root, width, height); return root;
    }
    private static void Layout(FrameworkElement root, int width, int height)
    {
        root.Measure(new Size(width, height)); root.Arrange(new Rect(0, 0, width, height)); root.UpdateLayout();
    }
    private static void Render(FrameworkElement root, int width, int height, double scale, string file)
    {
        Layout(root, width, height);
        var bitmap = new RenderTargetBitmap((int)(width * scale), (int)(height * scale), 96 * scale, 96 * scale, PixelFormats.Pbgra32);
        bitmap.Render(root); var png = new PngBitmapEncoder(); png.Frames.Add(BitmapFrame.Create(bitmap));
        using var output = File.Create(file); png.Save(output);
    }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject node)
    {
        yield return node;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(node); i++)
            foreach (var child in Descendants(VisualTreeHelper.GetChild(node, i))) yield return child;
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static void Check(FrameworkElement root, FakeSource source)
    {
        var nodes = Descendants(root).ToArray();
        var apply = nodes.OfType<Button>().Single(b => b.Content as string == "Apply");
        Require(!apply.IsEnabled, "Clean Apply must be disabled");
        var vigor = nodes.OfType<TextBox>().FirstOrDefault(b => AutomationProperties.GetName(b) == "Vigor");
        if (vigor != null)
        {
            vigor.Text = "42"; Require(apply.IsEnabled == !source.GameRunning, "Dirty Apply/game-running gate failed");
            Require(nodes.OfType<TextBlock>().Any(t => t.Text == "Level 96"), "Resulting level did not update");
            vigor.Text = "100"; Require(!apply.IsEnabled, "Invalid stat must disable Apply");
            vigor.Text = "28"; Require(!apply.IsEnabled, "Returning to original must clear dirty state");
            var increase = nodes.OfType<Button>().Single(b => AutomationProperties.GetName(b) == "Increase Vigor");
            increase.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            Require(vigor.Text == "29", "Stat stepper failed"); vigor.Text = "28";
            var name = nodes.OfType<TextBox>().Single(b => AutomationProperties.GetName(b) == "Name");
            name.Text = ""; Require(!apply.IsEnabled, "Empty name must disable Apply"); name.Text = "Evelyn";
        }
        foreach (var restore in nodes.OfType<Button>().Where(b => b.Content as string == "Restore"))
            Require(restore.IsEnabled == !source.GameRunning, "Restore game-running gate failed");
        var search = nodes.OfType<TextBox>().FirstOrDefault(b => AutomationProperties.GetName(b) == "Search items");
        if (search != null)
        {
            var count = nodes.OfType<TextBox>().First(b => AutomationProperties.GetName(b) == "Count");
            count.Text = "17"; Require(apply.IsEnabled, "Inventory change must enable Apply");
            search.Text = "no-match"; search.Text = "";
            root.UpdateLayout();
            Require(Descendants(root).OfType<TextBox>().Any(b => b.Text == "17"), "Filtering discarded item edits");
            count.Text = "12"; Require(!apply.IsEnabled, "Inventory reset must clear dirty state");
        }
        Require(source.Commands.All(c => c is "slots" or "show" or "backups"), "Preview attempted a write");
    }
    private sealed class FakeSource(bool empty, bool running) : ISaveEditorSource
    {
        public bool GameRunning => running;
        public List<string> Commands { get; } = new();
        public Task<string> Run(string command, string kind, int? slot = null, object? edit = null, string? backup = null)
        {
            Commands.Add(command);
            object result = command switch
            {
                "slots" => empty ? Array.Empty<object>() : new object[] {
                    new { slot = 0, name = "Evelyn", level = 82, echoes = 126450 },
                    new { slot = 1, name = "Rowan", level = 34, echoes = 8420 },
                    new { slot = 2, name = "Ash", level = 116, echoes = 980100 } },
                "show" => new { revision = "fixture", character = new { name = "Evelyn", level = 82, echoes = 126450, insight = 24, ng_cycle = 1,
                    stats = new { vigor = 28, endurance = 20, strength = 18, skill = 34, bloodtinge = 18, arcane = 14 } },
                    items = new[] {
                        new { section = 0, slot = 0, kind = "goods", number = 100, name = "Restorative tonic", count = 12, level = (int?)null },
                        new { section = 0, slot = 1, kind = "goods", number = 101, name = "Silver cartridge", count = 20, level = (int?)null },
                        new { section = 0, slot = 2, kind = "goods", number = 102, name = "Ember flask", count = 6, level = (int?)null },
                        new { section = 0, slot = 3, kind = "weapon", number = 200000, name = "Wayfarer's blade", count = 1, level = (int?)6 },
                        new { section = 0, slot = 4, kind = "armor", number = 3000, name = "Weathered cloak", count = 1, level = (int?)null } } },
                "backups" => empty ? Array.Empty<object>() : new object[] {
                    new { name = "20261029-143200-before-edit", path = "fixture", reason = "before-edit" },
                    new { name = "20261028-201500-before-restore", path = "fixture", reason = "before-restore" },
                    new { name = "20261027-091000-before-edit", path = "fixture", reason = "before-edit" } },
                _ => throw new InvalidOperationException("Fixture data source refuses writes")
            };
            return Task.FromResult(JsonSerializer.Serialize(result));
        }
    }
}
