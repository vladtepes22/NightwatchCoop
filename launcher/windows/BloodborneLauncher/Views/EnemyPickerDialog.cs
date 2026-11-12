using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using BloodborneLauncher.Core;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views;

// The randomizer's "Never use these enemies" and "Only use these enemies" lists: search, filter by kind and area, tick to
// leave an enemy out (or, for the only list, to keep it in). Nothing changes until OK. Models outside the catalogue (from a code or a mod) stay in the result as they were.
internal sealed class EnemyPickerDialog : Window
{
    private sealed record Entry(EnemyCatalog.Enemy Enemy, CheckBox Box);

    private readonly List<Entry> _entries = new();
    private readonly TextBox _search = new() { PlaceholderText = L.T("Search by name, area or model number"), PlaceholderEnabled = true };
    private readonly ComboBox _kind = new() { MinWidth = 140, Margin = new Thickness(10, 0, 0, 0) };
    private readonly ComboBox _area = new() { MinWidth = 190, Margin = new Thickness(10, 0, 0, 0) };
    private readonly TextBlock _count = LauncherPage.Text("", "Muted"), _empty = LauncherPage.Text(L.T("No enemy matches."), "Muted");
    private readonly int[] _outside;
    private readonly string[] _areas;
    private readonly bool _only;
    public List<int> Result { get; private set; } = new();

    /// <param name="only">The "Only use these enemies" list: ticked enemies are the only replacements.</param>
    public EnemyPickerDialog(IReadOnlyList<int> ticked, bool only = false)
    {
        _only = only;
        var known = EnemyCatalog.All.Select(e => e.Model).ToHashSet();
        _outside = ticked.Where(m => !known.Contains(m)).ToArray();
        Title = only ? L.T("Only use these enemies") : L.T("Never use these enemies"); Width = 640; Height = 680; MinWidth = 480; MinHeight = 420;
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
        top.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        top.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        top.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        AutomationProperties.SetName(_search, L.T("Search"));
        foreach (var text in new[] { L.T("All kinds"), L.T("Ordinary"), L.T("Difficult"), L.T("Ticked") }) _kind.Items.Add(text);
        _kind.SelectedIndex = 0; AutomationProperties.SetName(_kind, L.T("Kind"));
        _areas = EnemyCatalog.All.SelectMany(e => e.Areas).Distinct().OrderBy(a => a, StringComparer.CurrentCulture).ToArray();
        _area.Items.Add(L.T("All areas"));
        foreach (var area in _areas) _area.Items.Add(area);
        _area.SelectedIndex = 0; AutomationProperties.SetName(_area, L.T("Area"));
        Grid.SetColumn(_search, 0); top.Children.Add(_search);
        Grid.SetColumn(_kind, 1); top.Children.Add(_kind);
        Grid.SetColumn(_area, 2); top.Children.Add(_area);
        Put(top, 0);

        var hint = LauncherPage.Text(only
            ? L.T("Only ticked enemies replace others; with none ticked, every enemy can. Never use these enemies still wins. Names marked with ? are our best guess: the game has no names for ordinary enemies.")
            : L.T("Ticked enemies never appear in the world. Names marked with ? are our best guess: the game has no names for ordinary enemies."), "Muted");
        hint.Margin = new Thickness(0, 0, 0, 10); Put(hint, 1);

        var list = new StackPanel();
        var picked = ticked.ToHashSet();
        foreach (var enemy in EnemyCatalog.All.OrderBy(e => e.Name, StringComparer.CurrentCulture))
        {
            var name = new TextBlock { Text = enemy.Name + (enemy.Guess ? " ?" : ""), FontSize = 14 };
            name.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
            var meta = LauncherPage.Text(enemy.Code + (enemy.Difficult ? " · " + L.T("difficult") : "") + " · " + string.Join(", ", enemy.Areas), "Muted");
            var box = new CheckBox
            {
                IsChecked = picked.Contains(enemy.Model),
                Content = new StackPanel { Children = { name, meta } },
                Margin = new Thickness(4, 4, 4, 4),
                Padding = new Thickness(8, 0, 0, 0),
            };
            AutomationProperties.SetName(box, enemy.Name + " " + enemy.Code);
            box.Checked += (_, _) => UpdateCount();
            box.Unchecked += (_, _) => { UpdateCount(); if (_kind.SelectedIndex == 3) Filter(); };
            _entries.Add(new Entry(enemy, box));
            list.Children.Add(box);
        }
        var scroll = new ScrollViewer { Content = list, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        var host = new Grid();
        host.Children.Add(scroll);
        _empty.Margin = new Thickness(12, 24, 12, 0); _empty.VerticalAlignment = VerticalAlignment.Top; _empty.Visibility = Visibility.Collapsed;
        host.Children.Add(_empty);
        Put(host, 2);

        var bottom = new Grid { Margin = new Thickness(0, 14, 0, 0) };
        bottom.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        bottom.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        _count.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(_count, 0); bottom.Children.Add(_count);
        var clear = new Button { Content = L.T("Clear all"), Margin = new Thickness(0, 0, 8, 0) };
        clear.Click += (_, _) => { foreach (var entry in _entries) entry.Box.IsChecked = false; Filter(); };
        var cancel = new Button { Content = L.T("Cancel"), IsCancel = true, Margin = new Thickness(0, 0, 8, 0) };
        var ok = new Button { Content = L.T("OK"), IsDefault = true, Appearance = Wpf.Ui.Controls.ControlAppearance.Primary, MinWidth = 80 };
        ok.Click += (_, _) =>
        {
            Result = _entries.Where(e => e.Box.IsChecked == true).Select(e => e.Enemy.Model).Concat(_outside).Distinct().OrderBy(m => m).ToList();
            DialogResult = true;
        };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Children = { clear, cancel, ok } };
        Grid.SetColumn(buttons, 1); bottom.Children.Add(buttons);
        Put(bottom, 3);
        Content = body;

        _search.TextChanged += (_, _) => Filter();
        _kind.SelectionChanged += (_, _) => Filter();
        _area.SelectionChanged += (_, _) => Filter();
        Loaded += (_, _) => _search.Focus();
        Filter();
        UpdateCount();
    }

    private void Filter()
    {
        var words = (_search.Text ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
        var area = _area.SelectedIndex > 0 ? _areas[_area.SelectedIndex - 1] : null;
        var shown = 0;
        foreach (var (enemy, box) in _entries)
        {
            var haystack = enemy.Name + " " + enemy.Code + " " + string.Join(" ", enemy.Areas);
            var visible = words.All(w => haystack.Contains(w, StringComparison.CurrentCultureIgnoreCase))
                          && (area == null || enemy.Areas.Contains(area))
                          && _kind.SelectedIndex switch
                          {
                              1 => !enemy.Difficult,
                              2 => enemy.Difficult,
                              3 => box.IsChecked == true,
                              _ => true,
                          };
            box.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
            if (visible) shown++;
        }
        _empty.Visibility = shown == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdateCount()
    {
        var count = _entries.Count(e => e.Box.IsChecked == true) + _outside.Length;
        _count.Text = count == 0 ? L.T("Every enemy can appear.") : _only ? L.F($"Only {count}") : L.F($"{count} left out");
    }
}
