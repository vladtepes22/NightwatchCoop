using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Documents;
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
        if (args.Length != 1 && !(args.Length == 2 && args[1] == "--coop")) { Console.Error.WriteLine("Usage: SaveEditorShots <output-folder> [--coop]"); return 2; }
        try
        {
            Directory.CreateDirectory(args[0]);
            // Plain Application, never the launcher's App/Workspace startup path. No HWND is shown.
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            app.Resources.MergedDictionaries.Add(new ThemesDictionary { Theme = ApplicationTheme.Dark });
            app.Resources.MergedDictionaries.Add(new ControlsDictionary());
            foreach (var file in new[] { "Charcoal", "Styles", "ScrollBar" })
                app.Resources.MergedDictionaries.Add(new ResourceDictionary { Source = new Uri($"pack://application:,,,/Nightwatch Co-op;component/Themes/{file}.xaml") });
            if (args.Length == 2) { CoopShots(args[0]); app.Shutdown(); return 0; }
            int shots = 0;
            IconPath = WriteIcon(Path.Combine(args[0], "fixture-icon.png"));
            foreach (var theme in new[] { "default", "crimson", "moon" })
            {
                var settings = new LauncherSettings(); settings.Set("ui_theme", theme); LauncherTheme.Apply(settings, false);
                CheckActionButton("charcoal " + theme);
                foreach (var size in new[] { (900, 700), (1200, 800) })
                foreach (var state in new[] { "character", "inventory", "weapons", "gems", "progress", "backups", "empty", "running" })
                {
                    var source = new FakeSource(state == "empty", state == "running");
                    var window = new SaveEditorWindow(source);
                    window.LoadAsync().GetAwaiter().GetResult();
                    window.SelectTab(state is "inventory" or "weapons" or "gems" ? 1 : state == "progress" ? 2 : state is "backups" or "empty" ? 3 : 0);
                    var root = Mount(window, size.Item1, size.Item2);
                    if (state is "weapons" or "gems")
                    {
                        Descendants(root).OfType<Button>().First(b => b.Content as string == (state == "gems" ? "Gems" : "Right hand"))
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
            foreach (var size in new[] { (620, 640), (480, 480) })
            {
                var current = new[] { new SaveEditor.GemEffect(126301, "Physical ATK UP +9.1%"), new SaveEditor.GemEffect(28101, "ATK DOWN -0.3%") };
                var choices = Effects().Select(e => JsonSerializer.Deserialize<SaveEditor.GemEffect>(JsonSerializer.Serialize(e), SaveEditor.Json)!).ToArray();
                var dialog = new SaveEditorGemDialog("Cursed Tempering Blood Gemstone (2)", current, choices);
                var content = (FrameworkElement)dialog.Content; dialog.Content = null;
                var root = new Border { Child = content, Width = size.Item1, Height = size.Item2 };
                dialog.Content = root; root.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
                Layout(root, size.Item1, size.Item2);
                CheckGemDialog(root);
                Render(root, size.Item1, size.Item2, 1, Path.Combine(args[0], $"moon-{size.Item1}x{size.Item2}-gem-effects.png")); shots++; dialog.Close();
            }
            {
                // Add item: check boxes on the left, two rows ticked (a multiple pick), room for the scroll bar.
                var found = Enumerable.Range(0, 14).Select(i => new { Number = 2146 + i, Name = "Sample item " + (i + 1), Icon = (string?)IconPath, Category = 4, Shape = 1 });
                var json = JsonSerializer.Serialize(found, SaveEditor.Json);
                var dialog = new SaveEditorAddDialog((category, _) => Task.FromResult(category == 4 ? json : "[]"), new[] { (4, 2149) }); // the save holds item 4: its note shows
                var content = (FrameworkElement)dialog.Content; dialog.Content = null;
                var root = new Border { Child = content, Width = 600, Height = 660 };
                dialog.Content = root; root.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
                SynchronizationContext.SetSynchronizationContext(new System.Windows.Threading.DispatcherSynchronizationContext()); // awaits come back to this thread, as in the app
                var load = (Task)typeof(SaveEditorAddDialog).GetMethod("Load", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.Invoke(dialog, null)!;
                while (!load.IsCompleted) System.Windows.Threading.Dispatcher.CurrentDispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.Background);
                Layout(root, 600, 660);
                var list = Descendants(root).OfType<ListBox>().First();
                Require(list.Items.Count == 14, "Add item: the sample rows did not load: " + list.Items.Count + " rows, " + load.Status + " " + load.Exception?.GetBaseException().Message + " | " + string.Join(" | ", Descendants(root).OfType<TextBlock>().Select(b => b.Text).Where(s => s.Length > 0).Take(6)));
                list.SelectedItems.Add(list.Items[1]); list.SelectedItems.Add(list.Items[3]);
                Layout(root, 600, 660);
                var boxes = Descendants(list).OfType<CheckBox>().ToList();
                Require(boxes.Count > 3 && boxes[1].IsChecked == true && boxes[3].IsChecked == true && boxes[0].IsChecked != true, "Add item: the check boxes do not follow the pick");
                Require(Descendants(root).OfType<TextBlock>().Any(b => b.Text.StartsWith("Already in your save") && b.Parent is Border { Visibility: Visibility.Visible }), "Add item: the held-item note is not shown");
                Render(root, 600, 660, 1, Path.Combine(args[0], "moon-600x660-add-item.png")); shots++; dialog.Close();
            }
            Console.WriteLine($"PASS: {shots} off-screen PNGs; fixtures, dirty state, validation, filtering, gem effects and write protection checked.");
            app.Shutdown(); return 0;
        }
        catch (Exception e) { Console.Error.WriteLine(e); return 1; }
    }
    private static void CoopShots(string output)
    {
        Environment.SetEnvironmentVariable("BB_LAUNCHER_CONFIG_DIR", Path.Combine(output, "fixture-config"));
        var settings = Workspace.Current.Settings;
        settings.Set("party_stun", "off");
        settings.Set("party_name", "Example");
        var looks = LauncherTheme.Themes.Select(t => (Name: t.Value, Theme: t.Value, Colour: "")).Where(l => l.Theme != "custom").ToList();
        looks.AddRange(new[] { (Name: "custom-white", Theme: "custom", Colour: "#FFFFFF"), (Name: "custom-yellow", Theme: "custom", Colour: "#FFE000"), (Name: "custom-black", Theme: "custom", Colour: "#101010") });
        foreach (var look in looks)
        {
            settings.Set("ui_theme", look.Theme);
            settings.Set("ui_theme_colour", look.Colour);
            LauncherTheme.Apply(settings, true);
            var shade = LauncherTheme.ShadeColour(settings);
            foreach (var state in new[] { "host-public", "host-private", "join" })
            {
                settings.Set("party_mode", state == "join" ? "join" : "host");
                settings.Set("party_host_public", state == "host-public" ? "1" : "0");
                var page = new SeamlessPage(state == "host-public");
                // Sectionize without Loaded: no timers, listeners, VPN calls or windows.
                typeof(LauncherPage).GetMethod("Sectionize", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.Invoke(page, null);
                var content = (UIElement)page.Content; page.Content = null;
                var root = CoopRoot(content, 820, 700, shade);
                Layout(root, 820, 700);
                foreach (var control in Descendants(root).OfType<FrameworkElement>().Where(c => c is Button or TextBox && c.Visibility == Visibility.Visible && c.ActualWidth > 0))
                {
                    var bounds = control.TransformToAncestor(root).TransformBounds(new Rect(control.RenderSize));
                    Require(bounds.Left >= 0 && bounds.Right <= 820.5, "Co-op control clipped horizontally: " + look.Name + " " + state);
                }
                Render(root, 820, 700, 1, Path.Combine(output, "coop-" + look.Name + "-" + state + ".png"));
                Require(!Directory.Exists(Path.Combine(output, "fixture-config")), "Preview wrote configuration");
            }
            if (look.Name == looks[0].Name) { settings.Set("party_mode", "host"); settings.Set("party_host_public", "1"); PageShot(output, new SeamlessPage(true), "coop-tall-host", shade); }
            if (look.Name == looks[0].Name) PageShot(output, new ModsPage(), "mods", shade);
            LobbyShot(output, look.Name, settings, shade);
            CheckActionButton("coop " + look.Name);
            var primaryFill = ((SolidColorBrush)Application.Current.TryFindResource("AccentButtonBackground")).Color;
            var primaryText = ((SolidColorBrush)Application.Current.TryFindResource("AccentButtonForeground")).Color;
            var primaryContrast = Contrast(primaryText, primaryFill);
            Require(primaryContrast >= 3.5, $"{look.Name}: primary button contrast {primaryContrast:0.0}");
            Console.WriteLine($"{look.Name}: primary button #{primaryFill.R:X2}{primaryFill.G:X2}{primaryFill.B:X2} {primaryContrast:0.0}:1");
            if (look.Theme is not ("default" or "eldritch"))
            {
                var accent = LauncherTheme.Accent(settings)!.Value;
                var fill = LauncherTheme.SegmentFill(accent, true);
                var text = ((SolidColorBrush)Application.Current.Resources["SegmentSelectedTextBrush"]).Color;
                Require(text == LauncherTheme.SegmentText(fill), look.Name + ": pill text resource not applied");
                var pill = Contrast(text, fill);
                Require(pill >= 3.5, $"{look.Name}: selected pill contrast {pill:0.0}");
                Console.WriteLine($"{look.Name}: selected pill {pill:0.0}:1 ({(text == Colors.White ? "white" : "dark")} text)");
            }
        }
        Console.WriteLine($"PASS: {looks.Count * 4} off-screen Co-op PNGs in {looks.Count} looks; no clipped controls or configuration writes; lobby marks and text readable.");
    }

    /// <summary>A row's action button (SmallButton, Secondary) has a filled background, not the library's 6% white.</summary>
    private static void CheckActionButton(string look)
    {
        var button = LauncherPage.SmallButton("Action", Wpf.Ui.Controls.SymbolRegular.Add24, (_, _) => { });
        Require(button.Background is SolidColorBrush { Color.A: >= 0xB0 }, look + ": action button background is see-through");
        Require(button.MouseOverBackground is SolidColorBrush { Color.A: >= 0xB0 }, look + ": action button hover is see-through");
    }

    private static Border CoopRoot(UIElement content, int width, int height, Color shade)
    {
        var root = new Border { Child = content, Width = width, Height = height, Background = new SolidColorBrush(shade) };
        root.SetResourceReference(TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");
        return root;
    }

    /// <summary>Contrast ratio of a possibly translucent colour over an opaque one.</summary>
    private static double Contrast(Color front, Color back)
    {
        double a = front.A / 255.0;
        var over = Color.FromRgb((byte)(front.R * a + back.R * (1 - a)), (byte)(front.G * a + back.G * (1 - a)), (byte)(front.B * a + back.B * (1 - a)));
        double one = LauncherTheme.RelativeLuminance(over), two = LauncherTheme.RelativeLuminance(back);
        return (Math.Max(one, two) + 0.05) / (Math.Min(one, two) + 0.05);
    }

    /// <summary>The real lobby rows from fixture games: a public, a private, a full and another build's party.</summary>
    /// <summary>A whole page, tall, without Loaded (Host tab's lower rows, the Mods page).</summary>
    private static void PageShot(string output, LauncherPage page, string name, Color shade)
    {
        typeof(LauncherPage).GetMethod("Sectionize", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.Invoke(page, null);
        var content = (UIElement)page.Content; page.Content = null;
        var root = CoopRoot(content, 820, 1500, shade);
        Layout(root, 820, 1500);
        Render(root, 820, 1500, 1, Path.Combine(output, name + ".png"));
    }

    private static void LobbyShot(string output, string look, LauncherSettings settings, Color shade)
    {
        Environment.SetEnvironmentVariable("BB_PARTY_VPNLOBBY_ADDR", "127.0.0.1:1");
        settings.Set("party_mode", "join");
        var page = new SeamlessPage(false);
        var build = PartyDiscovery.BuildPrefix(Updater.ReleaseVersion()) + "fixture";
        LobbyGame Game(string name, int players, int max, bool password, string b, string mods = "") =>
            new() { Name = name, Players = players, Max = max, Password = password, Build = b, Own = false, Ip = 0x0a080001, Port = 1, Mods = mods };
        // No poller thread: the fixture list is all the page sees.
        var poller = (VpnLobby.Poller)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(VpnLobby.Poller));
        var open = new LobbyGame { Name = "Public", Players = 1, Max = 3, Password = false, Build = build, Own = false, Ip = 0x0a080001, Port = 1, Invaders = true };
        poller.Games = new List<LobbyGame> { open, Game("Private", 2, 3, true, build), Game("Full", 2, 2, true, build), Game("Other", 1, 3, false, "other-build"),
            Game("Nocturne host", 1, 3, false, build, "nocturne@9.9"), Game("Modded host", 1, 3, false, build, "mods:3f9a12c0") };
        poller.Answered = true;
        const System.Reflection.BindingFlags flags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        typeof(SeamlessPage).GetField("_lobby", flags)!.SetValue(page, poller);
        typeof(SeamlessPage).GetField("_lobbyOn", flags)!.SetValue(page, true);
        typeof(SeamlessPage).GetMethod("RefreshLobby", flags)!.Invoke(page, null);
        var list = (StackPanel)typeof(SeamlessPage).GetField("_lobbyList", flags)!.GetValue(page)!;
        Require(list.Children.Count == 6, "Lobby rows missing");
        switch (System.Windows.LogicalTreeHelper.GetParent(list))
        {
            case Panel panel: panel.Children.Remove(list); break;
            case Decorator decorator: decorator.Child = null; break; // the Join list's scroller (about 6 rows)
            case ContentControl control: control.Content = null; break;
        }
        var root = CoopRoot(list, 820, 520, shade);
        Layout(root, 820, 520);
        var marks = Descendants(root).OfType<Border>().Where(b => b.ToolTip is string && b.Visibility == Visibility.Visible && b.ActualWidth > 0).ToList();
        Require(marks.Count == 6, "Every lobby row needs one visible hover mark, found " + marks.Count);
        Require(!Descendants(root).OfType<Wpf.Ui.Controls.SymbolIcon>().Any(i => i.Symbol == Wpf.Ui.Controls.SymbolRegular.Info16), "Lobby rows must not show the info mark");
        var locks = Descendants(root).OfType<System.Windows.Shapes.Path>().Where(p => p.ActualWidth > 0 && p.Fill is SolidColorBrush && p.LayoutTransform == Transform.Identity).ToList();
        Require(locks.Count == 2, "Two padlocks expected, found " + locks.Count);
        var swords = Descendants(root).OfType<Button>().Count(b => b.Content is StackPanel s && s.Children.OfType<System.Windows.Shapes.Path>().Any() && b.Background is SolidColorBrush { Color.R: > 0x80 });
        Require(swords == 1, "One red Invade button with a sword expected, found " + swords);
        Require(Descendants(root).OfType<Wpf.Ui.Controls.SymbolIcon>().Count(i => i.Symbol == Wpf.Ui.Controls.SymbolRegular.Globe24) == 4, "Four globes expected");
        var mark = Contrast(((SolidColorBrush)locks[0].Fill).Color, shade);
        Require(mark >= 4.5, $"{look}: padlock contrast {mark:0.0}");
        var least = Descendants(root).OfType<TextBlock>().Where(t => t.Foreground is SolidColorBrush && t.Text.Length > 0).Min(t => Contrast(((SolidColorBrush)t.Foreground).Color, shade));
        Require(least >= 3, $"{look}: lobby text contrast {least:0.0}");
        Console.WriteLine($"{look}: padlock {mark:0.0}:1, faintest text {least:0.0}:1");
        // Checked by eye, on its own fill: a selected party's Join, as the local list draws it.
        list.Children.Add(LauncherPage.SmallButton("Selected", Wpf.Ui.Controls.SymbolRegular.Checkmark24, (_, _) => { }, Wpf.Ui.Controls.ControlAppearance.Primary));
        Layout(root, 820, 340);
        Render(root, 820, 520, 2, Path.Combine(output, "lobby-" + look + ".png"));
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
    private static string WriteIcon(string file)
    {
        var visual = new DrawingVisual();
        using (var dc = visual.RenderOpen())
            dc.DrawGeometry(Brushes.IndianRed, null, Geometry.Parse("M40,6 L74,40 L40,74 L6,40 Z"));
        var bitmap = new RenderTargetBitmap(80, 80, 96, 96, PixelFormats.Pbgra32); bitmap.Render(visual);
        var png = new PngBitmapEncoder(); png.Frames.Add(BitmapFrame.Create(bitmap));
        using (var output = File.Create(file)) png.Save(output);
        return file;
    }
    // The dialog's buttons follow the 1-3 effect rule; a choice replaces or joins the list.
    private static void CheckGemDialog(FrameworkElement root)
    {
        Button Named(string text) => Descendants(root).OfType<Button>().Single(b => b.Content as string == text);
        void Click(string text) => Named(text).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        var lists = Descendants(root).OfType<ListBox>().ToArray();
        var current = lists.Single(l => AutomationProperties.GetName(l) == "Effects");
        var choices = lists.Single(l => AutomationProperties.GetName(l) == "All blood gem effects");
        Require(current.Items.Count == 2 && Named("OK").IsEnabled, "Gem dialog did not load the effects");
        Require(!Named("Add effect").IsEnabled, "Add needs a chosen effect");
        var search = Descendants(root).OfType<TextBox>().Single(b => AutomationProperties.GetName(b) == "Search effects");
        search.Text = "Fire"; Require(choices.Items.Count == 100, "Effect search did not filter");
        choices.SelectedIndex = 0; Click("Add effect");
        Require(current.Items.Count == 3 && !Named("Add effect").IsEnabled, "A gem takes at most 3 effects");
        Click("Remove effect"); Click("Remove effect");
        Require(current.Items.Count == 1 && !Named("Remove effect").IsEnabled, "A gem keeps at least one effect");
        Click("Replace selected");
        Require(((SaveEditor.GemEffect)current.Items[0]).Text!.StartsWith("Fire"), "Replace did not change the effect");
        search.Text = "";
    }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject node)
    {
        yield return node;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(node); i++)
            foreach (var child in Descendants(VisualTreeHelper.GetChild(node, i))) yield return child;
    }
    /// The window filters after a typing pause (SaveEditorWindow._searchPause, 180 ms): let the dispatcher run past it.
    private static void WaitSearchPause(FrameworkElement root)
    {
        var until = DateTime.UtcNow.AddMilliseconds(400);
        while (DateTime.UtcNow < until)
        {
            var frame = new System.Windows.Threading.DispatcherFrame();
            System.Windows.Threading.Dispatcher.CurrentDispatcher.BeginInvoke(
                System.Windows.Threading.DispatcherPriority.Background, new Action(() => frame.Continue = false));
            System.Windows.Threading.Dispatcher.PushFrame(frame);
            Thread.Sleep(10);
        }
        root.UpdateLayout();
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
            search.Text = "no-match"; WaitSearchPause(root);
            Require(!Descendants(root).OfType<TextBox>().Any(b => AutomationProperties.GetName(b) == "Count"), "Search did not filter the list");
            search.Text = ""; WaitSearchPause(root);
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
        rows.Add(new { section = 0, slot = 40, kind = "gem", number = 124001, name = "Cursed Tempering Blood Gemstone (2)", count = 1, level = (int?)null, max_level = (int?)null,
            icon = IconPath, gem = new { handle = 3229614081L, type = 1, shape = 1, sort = 0, cursed = true,
                effects = new object[] { new { id = 126301, text = "Physical ATK UP +9.1%" }, new { id = 28101, text = "ATK DOWN -0.3%" } } } });
        rows.Add(new { section = 0, slot = 41, kind = "gem", number = 129001, name = "Moon", count = 1, level = (int?)null, max_level = (int?)null,
            icon = IconPath, gem = new { handle = 3229614082L, type = 2, shape = 63, sort = 1, cursed = false,
                effects = new object[] { new { id = 600, text = "Increase Blood Echoes gained +10%" }, new { id = 601, text = (string?)null } } } });
        rows.Add(new { section = 3, slot = 0, kind = "gem", number = 124001, name = "Adept Blood Gemstone (1)", count = 1, level = (int?)null, max_level = (int?)null,
            icon = IconPath, locked = true, gem = new { handle = 3229614083L, type = 1, shape = 2, sort = 0, cursed = false,
                effects = new object[] { new { id = 13105, text = "Blunt ATK UP +3.8%" } }, socketed_in = new { number = 1000100, name = "Saw Cleaver" } } });
        rows[10] = new { section = 0, slot = 20, kind = "weapon", number = 1000300, name = "Saw Cleaver", count = 1, level = (int?)3, max_level = (int?)10, icon = IconPath,
            sockets = new object[] { new { shape = 2, gem = "Adept Blood Gemstone (1)" }, new { shape = 1, gem = (string?)null } } };
        rows.Add(new { section = 1, slot = 0, kind = "goods", number = 4000, name = "Iron Key", count = 1, level = (int?)null, max_level = (int?)null });
        rows.Add(new { section = 1, slot = 1, kind = "goods", number = 4001, name = "Beckoning Bell", count = 1, level = (int?)null, max_level = (int?)null });
        return rows.ToArray();
    }
    private static string? IconPath;
    private static object[] Effects() => Enumerable.Range(1, 400)
        .Select(i => (object)new { id = 13100 + i, text = (i % 4) switch { 0 => "Blunt", 1 => "Thrust", 2 => "Fire", _ => "Arcane" } + " ATK UP +" + (i / 10.0).ToString("0.0") + "%" })
        .ToArray();
    private static object[] Flags()
    {
        var rows = new List<object>();
        string[] bosses = { "Cleric Beast", "Father Gascoigne", "Blood-starved Beast", "Vicar Amelia", "Witch of Hemwick" };
        for (int i = 0; i < bosses.Length; i++)
            rows.Add(new { id = 12101800 + i, kind = "boss", group = i < 3 ? "Central Yharnam" : "Cathedral Ward", label = bosses[i] + " [" + (12101800 + i) + "]", on = i < 2 });
        string[] lamps = { "Hunter's Dream", "Central Yharnam", "Cathedral Ward", "Old Yharnam", "Hemwick Charnel Lane", "Forbidden Woods" };
        for (int i = 0; i < lamps.Length; i++)
            rows.Add(new { id = 72110200 + i, kind = "lamp", group = i < 3 ? "Yharnam" : "Beyond", label = lamps[i] + " [" + (72110200 + i) + "]", on = i != 4 });
        rows.Add(new { id = 6600, kind = "ending", group = "Endings", label = "Yharnam Sunrise seen", on = false });
        rows.Add(new { id = 6603, kind = "ending", group = "Endings", label = "Any ending seen", on = false });
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
                "effects" => Effects(),
                _ => throw new InvalidOperationException("Fixture data source refuses writes")
            };
            return Task.FromResult(JsonSerializer.Serialize(result));
        }
    }
}
