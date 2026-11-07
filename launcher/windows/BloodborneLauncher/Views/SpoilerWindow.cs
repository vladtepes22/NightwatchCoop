using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views;

/// <summary>A world's spoiler in words: one tab per kind of change (enemies, bosses, treasure, shops, drops, starting
/// weapons), each a list of where, what was there and what is there now, with a search over all three.</summary>
public sealed class SpoilerWindow : Window
{
    private sealed record Line(string Where, string Was, string Now);

    private readonly List<(string Title, List<Line> Lines)> _tabs = new();
    private readonly StackPanel _list = new();
    private readonly TextBlock _count = LauncherPage.Text("", "Muted");
    private readonly TextBox _search = new() { PlaceholderText = L.T("Search areas, enemies and items"), PlaceholderEnabled = true };
    private readonly DispatcherTimer _pause = new() { Interval = TimeSpan.FromMilliseconds(180) };
    private int _tab;

    public SpoilerWindow(string code, WorldSpoiler spoiler)
    {
        if (Application.Current.MainWindow is { } owner && owner != this) Owner = owner;
        Title = L.T("Spoiler") + " " + code; Width = 900; Height = 700; MinWidth = 560; MinHeight = 420;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        SetResourceReference(BackgroundProperty, "DialogBackgroundBrush");
        SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
        SetResourceReference(FontFamilyProperty, "UiFont");
        UseLayoutRounding = true;
        SourceInitialized += (_, _) => WindowTheme.Apply(this);

        string Model(string m) => WorldSpoiler.ModelText(spoiler.Models, m);
        Add(L.T("Enemies"), spoiler.Enemies.Select(e => new Line(e.Map, Model(e.From), Model(e.To) + (e.Count > 1 ? $" ×{e.Count}" : ""))));
        Add(L.T("Bosses"), spoiler.Bosses.Select(b => new Line(b.Map, Model(b.From), Model(b.To))));
        Add(L.T("Treasure"), spoiler.Treasure.Select(Of));
        Add(L.T("Shops"), spoiler.Shops.Select(Of));
        Add(L.T("Drops"), spoiler.Drops.Select(Of));
        Add(L.T("Starting weapons"), spoiler.Starters.Select(Of));

        var body = new DockPanel { Margin = new Thickness(24) };
        var heading = new StackPanel { Margin = new Thickness(0, 0, 0, 14) };
        heading.Children.Add(LauncherPage.Text(L.T("Spoiler"), "PageTitle"));
        heading.Children.Add(LauncherPage.Text(code + " · " + L.T("Names come from your game's English text. Enemies the game gives no name keep their model number."), "Muted"));
        if (spoiler.Error != null) heading.Children.Add(LauncherPage.Text(L.T("Some names could not be read: ") + spoiler.Error, "Muted"));
        DockPanel.SetDock(heading, Dock.Top);
        body.Children.Add(heading);

        var tools = new StackPanel { Margin = new Thickness(0, 0, 0, 12) };
        if (_tabs.Count > 1)
        {
            var tabs = new Segmented(_tabs.Select(t => new Choice($"{t.Title} · {t.Lines.Count}", t.Title)).ToList());
            tabs.Select(0, notify: false);
            tabs.Changed += index => { _tab = index; Fill(); };
            tabs.Margin = new Thickness(0, 0, 0, 10);
            tools.Children.Add(tabs);
        }
        tools.Children.Add(_search);
        _count.Margin = new Thickness(2, 8, 0, 0);
        tools.Children.Add(_count);
        DockPanel.SetDock(tools, Dock.Top);
        body.Children.Add(tools);

        if (_tabs.Count == 0)
            body.Children.Add(LauncherPage.Text(L.T("Nothing changed in this world."), "Muted"));
        else
            body.Children.Add(new ScrollViewer { Content = _list, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        Content = body;

        _search.TextChanged += (_, _) => { _pause.Stop(); _pause.Start(); };
        _pause.Tick += (_, _) => { _pause.Stop(); Fill(); };
        Fill();
    }

    private static Line Of(WorldSpoiler.Trade t) => new(t.Where, t.Was, t.Now);

    private void Add(string title, IEnumerable<Line> lines)
    {
        var list = lines.ToList();
        if (list.Count > 0) _tabs.Add((title, list));
    }

    private void Fill()
    {
        _list.Children.Clear();
        if (_tabs.Count == 0) return;
        var words = _search.Text.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        var lines = _tabs[_tab].Lines.Where(l => words.All(w => (l.Where + " " + l.Was + " " + l.Now).Contains(w, StringComparison.OrdinalIgnoreCase))).ToList();
        _list.Children.Add(Row(L.T("Where"), L.T("Was"), L.T("Now"), header: true));
        foreach (var line in lines) _list.Children.Add(Row(line.Where, line.Was, line.Now, header: false));
        _count.Text = lines.Count == _tabs[_tab].Lines.Count ? L.F($"{lines.Count} changes") : L.F($"{lines.Count} of {_tabs[_tab].Lines.Count} changes");
    }

    private static Grid Row(string where, string was, string now, bool header)
    {
        var grid = new Grid { Margin = new Thickness(0, 0, 0, header ? 6 : 2) };
        foreach (var width in new[] { 0.9, 1.2, 1.2 })
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(width, GridUnitType.Star) });
        var cells = new[] { where, was, (header ? "" : "→ ") + now };
        for (var i = 0; i < cells.Length; i++)
        {
            var text = LauncherPage.Text(cells[i], header ? "StatLabel" : "Muted");
            text.TextWrapping = TextWrapping.Wrap;
            text.Margin = new Thickness(0, 0, 12, 0);
            if (!header && i == 2) text.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
            Grid.SetColumn(text, i);
            grid.Children.Add(text);
        }
        return grid;
    }
}
