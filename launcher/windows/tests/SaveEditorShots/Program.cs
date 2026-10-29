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
                foreach (var state in new[] { "character", "inventory", "weapons", "progress", "repairs", "backups", "empty", "running" })
                {
                    var source = new FakeSource(state == "empty", state == "running");
                    var window = new SaveEditorWindow(source);
                    window.LoadAsync().GetAwaiter().GetResult();
                    window.SelectTab(state is "inventory" or "weapons" ? 1 : state == "progress" ? 2 : state == "repairs" ? 3 : state is "backups" or "empty" ? 4 : 0);
                    var root = Mount(window, size.Item1, size.Item2);
                    if (state == "weapons")
                    {
                        Descendants(root).OfType<Button>().First(b => b.Content as string == "Weapons")
                            .RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
                        root.UpdateLayout();
                    }
                    Check(root, source);
                    Render(root, size.Item1, size.Item2, 1, Path.Combine(args[0], $"{theme}-{size.Item1}x{size.Item2}-{state}.png"));
                    shots++; window.Close();
                }
            }
            foreach (var scale in new[] { 1.25, 1.5 })
            foreach (var tab in new[] { 0, 1, 2, 3, 4 })
            {
                var window = new SaveEditorWindow(new FakeSource(false, false)); window.LoadAsync().GetAwaiter().GetResult();
                window.SelectTab(tab);
                var root = Mount(window, 700, 700);
                string state = tab == 0 ? "" : tab == 1 ? "inventory-" : tab == 2 ? "progress-" : tab == 3 ? "repairs-" : "backups-";
                Render(root, 700, 700, scale, Path.Combine(args[0], $"moon-700x700-{state}dpi{scale * 100:0}.png")); shots++; window.Close();
            }
            {
                var window = new SaveEditorWindow(new FakeSource(false, false)); window.LoadAsync().GetAwaiter().GetResult();
                var root = Mount(window, 1400, 800);
                var settings = new LauncherSettings(); settings.Set("ui_theme", "crimson"); LauncherTheme.Apply(settings, false);
                System.Windows.Threading.Dispatcher.CurrentDispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                root.UpdateLayout();
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
            Require(nodes.OfType<TextBlock>().Any(t => t.Text == "Level 82 → 96"), "Resulting level did not update");
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
            var before = count.Text;
            count.Text = "1234"; Require(apply.IsEnabled == !source.GameRunning, "Inventory change must enable Apply");
            search.Text = "no-match"; root.UpdateLayout();
            Require(!Descendants(root).OfType<TextBox>().Any(b => AutomationProperties.GetName(b) == "Count"), "Search did not filter the list");
            search.Text = ""; root.UpdateLayout();
            count = Descendants(root).OfType<TextBox>().FirstOrDefault(b => b.Text == "1234")!;
            Require(count != null, "Filtering discarded item edits");
            count.Text = before; Require(!apply.IsEnabled, "Inventory reset must clear dirty state");
            count.Text = "0"; Require(!apply.IsEnabled, "A zero count must disable Apply");
            count.Text = before;
            root.UpdateLayout(); nodes = Descendants(root).ToArray();
            var up = nodes.OfType<Button>().FirstOrDefault(b => AutomationProperties.GetName(b) == "Increase weapon level" && b.Command?.CanExecute(null) == true);
            if (up != null)
            {
                up.Command.Execute(null); Require(apply.IsEnabled == !source.GameRunning, "A weapon level change must enable Apply");
                var down = nodes.OfType<Button>().First(b => AutomationProperties.GetName(b) == "Decrease weapon level" && b.DataContext == up.DataContext);
                down.Command.Execute(null); Require(!apply.IsEnabled, "Returning the weapon level must clear dirty state");
            }
        }
        var toggle = nodes.OfType<Wpf.Ui.Controls.ToggleSwitch>().FirstOrDefault();
        if (toggle != null)
        {
            Require(toggle.IsChecked == true, "A set flag must load switched on");
            toggle.IsChecked = !toggle.IsChecked; Require(apply.IsEnabled == !source.GameRunning, "A progress flag must enable Apply");
            toggle.IsChecked = !toggle.IsChecked; Require(!apply.IsEnabled, "Returning a flag must clear dirty state");
        }
        var repair = nodes.OfType<System.Windows.Controls.CheckBox>().FirstOrDefault(b => b.Content as string == "Repair when applying");
        if (repair != null)
        {
            repair.IsChecked = true; repair.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            Require(apply.IsEnabled == !source.GameRunning, "A repair must enable Apply");
            repair.IsChecked = false; repair.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            Require(!apply.IsEnabled, "Unchecking a repair must clear dirty state");
        }
        Require(source.Commands.All(c => c is "slots" or "show" or "backups"), "Preview attempted a write");
    }

    private static object[] Items()
    {
        var rows = new List<object>();
        string[] goods = { "Blood Vial", "Quicksilver Bullet", "Molotov Cocktail", "Pebble", "Throwing Knife", "Antidote", "Sedative", "Blue Elixir", "Rally Potion", "Fire Paper" };
        for (int i = 0; i < goods.Length; i++)
            rows.Add(new { section = 0, slot = i, kind = "goods", number = 100 + i, name = goods[i], count = 5 + i * 3, level = (int?)null, max_level = (int?)null });
        string[] weapons = { "Saw Cleaver", "Threaded Cane", "Hunter Axe", "Ludwig's Holy Blade" };
        for (int i = 0; i < weapons.Length; i++)
            rows.Add(new { section = 0, slot = 20 + i, kind = "weapon", number = 1000000 + i * 1000 + (i % 3) * 100, name = weapons[i], count = 1, level = (int?)(i % 3 + 3), max_level = (int?)10 });
        string[] armor = { "Hunter Garb", "Yharnam Hunter Hat", "Old Hunter Trousers" };
        for (int i = 0; i < armor.Length; i++)
            rows.Add(new { section = 0, slot = 30 + i, kind = "armor", number = 300000 + i, name = armor[i], count = 1, level = (int?)null, max_level = (int?)null });
        rows.Add(new { section = 0, slot = 40, kind = "gem", number = 9000, name = "Radial Blood Gem", count = 1, level = (int?)null, max_level = (int?)null });
        rows.Add(new { section = 1, slot = 0, kind = "goods", number = 4000, name = "Iron Key", count = 1, level = (int?)null, max_level = (int?)null });
        rows.Add(new { section = 1, slot = 1, kind = "goods", number = 4001, name = "Beckoning Bell", count = 1, level = (int?)null, max_level = (int?)null });
        return rows.ToArray();
    }
    private static object[] Flags()
    {
        var rows = new List<object>();
        string[] bosses = { "Cleric Beast", "Father Gascoigne", "Blood-starved Beast", "Vicar Amelia", "Witch of Hemwick" };
        for (int i = 0; i < bosses.Length; i++)
            rows.Add(new { id = 12101800 + i, kind = "boss", group = i < 3 ? "Central Yharnam" : "Cathedral Ward", label = bosses[i] + " [" + (12101800 + i) + "]", on = i < 2 });
        string[] lamps = { "Hunter's Dream", "Central Yharnam", "Cathedral Ward", "Old Yharnam", "Hemwick Charnel Lane", "Forbidden Woods" };
        for (int i = 0; i < lamps.Length; i++)
            rows.Add(new { id = 72110200 + i, kind = "lamp", group = i < 3 ? "Yharnam" : "Beyond", label = lamps[i] + " [" + (72110200 + i) + "]", on = i != 4 });
        rows.Add(new { id = 21, kind = "ending", group = "Endings", label = "Ending flag [21]", on = false });
        return rows.ToArray();
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
                    items = Items(), flags = Flags(), findings = new { phantom = true, duplicate_keys = new[] { "Beckoning Bell" }, world_empty = false } },
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
