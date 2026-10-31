using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views;

// Picks items to add to the open save from the game's own item list. Nothing is written here: the save editor
// window keeps the picks as unsaved changes until Apply.
internal sealed class SaveEditorAddDialog : Window
{
    internal sealed record Pick(int Category, int Number, string Name, int Count);

    public sealed class Row(int number, string name, ImageSource? icon)
    {
        public int Number { get; } = number;
        public string Name { get; } = name;
        public ImageSource? Icon { get; } = icon;
        public string Meta => L.T("Item ID") + " " + Number;
        public Visibility IconVisibility => Icon != null ? Visibility.Visible : Visibility.Collapsed;
    }
    private sealed record Found(int Number, string? Name, string? Icon);

    // The categories the Python side offers: 4 goods, 0 weapons, 1 armour.
    private static readonly int[] Categories = { 4, 0, 1 };
    private const int SearchPauseMs = 200;
    private readonly Func<int, string, Task<string>> _catalog;
    private readonly ComboBox _kind = new();
    private readonly TextBox _search = new(), _count = new() { Text = "1", Width = 70 };
    private readonly ListBox _list = new();
    private readonly TextBlock _empty = LauncherPage.Text("", "Muted"), _countLabel = LauncherPage.Text(L.T("Count"), "StatLabel");
    private readonly Button _ok;
    private readonly HashSet<(int, int)> _held;
    private int _run;
    // Decoded once per file: a search shows mostly the icons the last one did.
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, ImageSource?> Icons = new();
    public Pick[] Result { get; private set; } = [];

    /// <param name="catalog">Asks the save editor's worker for the items of a category matching a text (JSON rows).</param>
    /// <param name="held">(category, number) the open save already holds as goods: those are edited in the list, not added.</param>
    public SaveEditorAddDialog(Func<int, string, Task<string>> catalog, IEnumerable<(int Category, int Number)> held)
    {
        _catalog = catalog; _held = held.ToHashSet();
        Title = L.T("Add item"); Width = 600; Height = 660; MinWidth = 460; MinHeight = 460;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        SetResourceReference(BackgroundProperty, "DialogBackgroundBrush");
        SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
        SetResourceReference(FontFamilyProperty, "UiFont");
        UseLayoutRounding = true; SourceInitialized += (_, _) => WindowTheme.Apply(this);
        var body = new Grid { Margin = new Thickness(22) };
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto })
            body.RowDefinitions.Add(new() { Height = height });
        void Put(UIElement element, int row) { Grid.SetRow(element, row); body.Children.Add(element); }

        var top = new Grid { Margin = new Thickness(0, 0, 0, 10) };
        top.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        top.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        foreach (var text in new[] { L.T("Goods"), L.T("Weapons"), L.T("Armor") }) _kind.Items.Add(text);
        _kind.SelectedIndex = 0; _kind.MinWidth = 150; _kind.Margin = new Thickness(0, 0, 12, 0);
        AutomationProperties.SetName(_kind, L.T("Add item"));
        Grid.SetColumn(_kind, 0); top.Children.Add(_kind);
        _search.PlaceholderText = L.T("Search items"); AutomationProperties.SetName(_search, L.T("Search items"));
        Grid.SetColumn(_search, 1); top.Children.Add(_search);
        Put(top, 0);
        var hint = LauncherPage.Text(L.T("Pick one or more items, then Add. Blood gems and runes cannot be added yet."), "Muted");
        hint.TextWrapping = TextWrapping.Wrap; hint.Margin = new Thickness(0, 0, 0, 10); Put(hint, 1);

        _list.SelectionMode = SelectionMode.Extended; _list.ItemTemplate = RowTemplate();
        _list.BorderThickness = new Thickness(0); _list.Background = Brushes.Transparent;
        ScrollViewer.SetHorizontalScrollBarVisibility(_list, ScrollBarVisibility.Disabled);
        VirtualizingPanel.SetIsVirtualizing(_list, true);
        AutomationProperties.SetName(_list, L.T("Add item"));
        _list.SelectionChanged += (_, _) => Update(); _list.MouseDoubleClick += (_, _) => { if (_ok!.IsEnabled) Accept(); };
        var host = new Grid(); host.Children.Add(_list);
        _empty.Margin = new Thickness(12, 24, 12, 0); _empty.VerticalAlignment = VerticalAlignment.Top; host.Children.Add(_empty);
        Put(host, 2);

        var footer = new Grid { Margin = new Thickness(0, 14, 0, 0) };
        foreach (var width in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto })
            footer.ColumnDefinitions.Add(new() { Width = width });
        void Place(UIElement element, int column) { Grid.SetColumn(element, column); footer.Children.Add(element); }
        _countLabel.VerticalAlignment = VerticalAlignment.Center; _countLabel.Margin = new Thickness(0, 0, 8, 0); Place(_countLabel, 0);
        AutomationProperties.SetName(_count, L.T("Count")); Place(_count, 1);
        var cancel = new Button { Content = L.T("Cancel"), MinWidth = 80, Margin = new Thickness(0, 0, 8, 0) };
        cancel.Click += (_, _) => Close(); Place(cancel, 3);
        _ok = new Button { Content = L.T("Add"), Appearance = ControlAppearance.Primary, MinWidth = 90, IsEnabled = false };
        _ok.Click += (_, _) => Accept(); Place(_ok, 4);
        Put(footer, 3);
        Content = body;

        _kind.SelectionChanged += async (_, _) => { Update(); await Load(); };
        // One search when typing pauses, not one per key: each asks the worker (requests queue) and decodes up to 60 icons.
        _search.TextChanged += async (_, _) =>
        {
            var typed = ++_run;
            await Task.Delay(SearchPauseMs);
            if (typed == _run) await Load();
        };
        _count.TextChanged += (_, _) => Update();
        PreviewKeyDown += (_, e) => { if (e.Key == Key.Escape) { e.Handled = true; Close(); } };
        Loaded += async (_, _) => await Load();
    }

    private int Category => Categories[Math.Max(0, _kind.SelectedIndex)];
    private bool IsGoods => Category == 4;
    private int? CountNumber => int.TryParse(_count.Text.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var n) && n is >= 1 and <= 99 ? n : null;

    private static DataTemplate RowTemplate() => (DataTemplate)XamlReader.Parse(
        "<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>" +
        "<Grid Margin='4,3'><Grid.ColumnDefinitions><ColumnDefinition Width='Auto'/><ColumnDefinition Width='*'/></Grid.ColumnDefinitions>" +
        "<Image Source='{Binding Icon}' Width='36' Height='36' Margin='0,0,12,0' Visibility='{Binding IconVisibility}'/>" +
        "<StackPanel Grid.Column='1' VerticalAlignment='Center'><TextBlock Text='{Binding Name}' TextTrimming='CharacterEllipsis'/>" +
        "<TextBlock Text='{Binding Meta}' Opacity='0.65' FontSize='12'/></StackPanel></Grid></DataTemplate>");

    private async Task Load()
    {
        var run = ++_run;
        var category = Category;
        try
        {
            var json = await _catalog(category, _search.Text.Trim());
            if (run != _run) return;
            var found = JsonSerializer.Deserialize<Found[]>(json, SaveEditor.Json) ?? [];
            var rows = await Task.Run(() => found.Select(f => new Row(f.Number, f.Name ?? "", CachedIcon(f.Icon))).ToList());
            if (run != _run) return;
            _list.ItemsSource = rows;
            _empty.Text = rows.Count == 0 ? L.T("No items found") : "";
        }
        catch (Exception e) when (e is InvalidOperationException or IOException or JsonException)
        {
            if (run != _run) return;
            _list.ItemsSource = null; _empty.Text = L.T("The game's item list could not be read. Check the game folder in Settings.");
        }
        Update();
    }
    private static ImageSource? CachedIcon(string? path)
    {
        if (string.IsNullOrEmpty(path)) return null;
        if (Icons.TryGetValue(path, out var known)) return known;
        var icon = ItemRow.LoadIcon(path);  // a file that failed is tried again next time: it may have been half written
        if (icon != null) Icons[path] = icon;
        return icon;
    }

    private void Update()
    {
        if (_ok == null) return;
        _countLabel.Visibility = _count.Visibility = IsGoods ? Visibility.Visible : Visibility.Collapsed;
        var chosen = _list.SelectedItems.Cast<Row>().ToList();
        _ok.IsEnabled = chosen.Count > 0 && (!IsGoods || CountNumber != null) &&
            (!IsGoods || chosen.All(r => !_held.Contains((4, r.Number))));
        _empty.Text = _list.ItemsSource is List<Row> { Count: 0 } ? L.T("No items found") :
            IsGoods && chosen.Any(r => _held.Contains((4, r.Number))) ? L.T("The save already holds an item you picked: change its count in the list instead.") : "";
    }
    private void Accept()
    {
        var count = IsGoods ? CountNumber ?? 1 : 1;
        Result = _list.SelectedItems.Cast<Row>().Select(r => new Pick(Category, r.Number, r.Name, count)).ToArray();
        DialogResult = true;
    }
}
