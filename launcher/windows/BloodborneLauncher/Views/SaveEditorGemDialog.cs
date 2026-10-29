using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views;

// Picks a blood gem's effects (1 to 3) from the game's own effect list. The first effect names the gem and
// picks its icon; one cursed effect makes it a cursed gem. Nothing is written here: the row keeps the result.
internal sealed class SaveEditorGemDialog : Window
{
    private readonly List<SaveEditor.GemEffect> _effects;
    private readonly ListBox _current = new(), _choices = new();
    private readonly TextBox _search = new();
    private readonly Button _replace, _add, _remove, _up, _ok;
    public SaveEditor.GemEffect[]? Result { get; private set; }

    public SaveEditorGemDialog(string gem, IReadOnlyList<SaveEditor.GemEffect> current, IReadOnlyList<SaveEditor.GemEffect> choices)
    {
        _effects = current.ToList();
        Title = L.T("Edit effects"); Width = 620; Height = 640; MinWidth = 480; MinHeight = 480;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        SetResourceReference(BackgroundProperty, "DialogBackgroundBrush");
        SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
        SetResourceReference(FontFamilyProperty, "UiFont");
        UseLayoutRounding = true;
        var body = new Grid { Margin = new Thickness(22) };
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, GridLength.Auto, GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto })
            body.RowDefinitions.Add(new() { Height = height });
        void Add(UIElement element, int row) { Grid.SetRow(element, row); body.Children.Add(element); }
        var heading = new StackPanel { Margin = new Thickness(0, 0, 0, 12) };
        var name = LauncherPage.Text(gem, "PageTitle"); name.TextTrimming = TextTrimming.CharacterEllipsis; heading.Children.Add(name);
        var hint = LauncherPage.Text(L.T("Up to 3 effects. The first one decides the gem's name and icon; a cursed effect makes it a cursed gem."), "Muted");
        hint.TextWrapping = TextWrapping.Wrap; heading.Children.Add(hint);
        Add(heading, 0);

        _current.DisplayMemberPath = nameof(SaveEditor.GemEffect.Text); _current.MinHeight = 96;
        AutomationProperties.SetName(_current, L.T("Effects"));
        _current.SelectionChanged += (_, _) => UpdateButtons();
        Add(_current, 1);
        var tools = new WrapPanel { Margin = new Thickness(0, 8, 0, 14) };
        _up = Small(L.T("Move up"), () => Move());
        _remove = Small(L.T("Remove effect"), () => { _effects.RemoveAt(_current.SelectedIndex); Show(Math.Max(0, _current.SelectedIndex - 1)); });
        tools.Children.Add(_up); tools.Children.Add(_remove); Add(tools, 2);

        Add(LauncherPage.Text(L.T("All blood gem effects"), "GroupHeader"), 3);
        _search.PlaceholderText = L.T("Search effects"); AutomationProperties.SetName(_search, L.T("Search effects"));
        _search.Margin = new Thickness(0, 0, 0, 8); Add(_search, 4);
        var view = (ListCollectionView)CollectionViewSource.GetDefaultView(choices.ToList());
        view.Filter = o => o is SaveEditor.GemEffect e && (_search.Text.Length == 0 ||
            (e.Text ?? "").Contains(_search.Text.Trim(), StringComparison.CurrentCultureIgnoreCase));
        _search.TextChanged += (_, _) => view.Refresh();
        _choices.ItemsSource = view; _choices.DisplayMemberPath = nameof(SaveEditor.GemEffect.Text);
        AutomationProperties.SetName(_choices, L.T("All blood gem effects"));
        VirtualizingPanel.SetIsVirtualizing(_choices, true); ScrollViewer.SetHorizontalScrollBarVisibility(_choices, ScrollBarVisibility.Disabled);
        _choices.SelectionChanged += (_, _) => UpdateButtons();
        _choices.MouseDoubleClick += (_, _) => { if (_replace.IsEnabled) Replace(); else if (_add.IsEnabled) Append(); };
        Add(_choices, 5);

        var footer = new Grid { Margin = new Thickness(0, 14, 0, 0) };
        foreach (var width in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto })
            footer.ColumnDefinitions.Add(new() { Width = width });
        void Place(UIElement element, int column) { Grid.SetColumn(element, column); footer.Children.Add(element); }
        _replace = Small(L.T("Replace selected"), Replace); Place(_replace, 0);
        _add = Small(L.T("Add effect"), Append); Place(_add, 1);
        var cancel = new Button { Content = L.T("Cancel"), MinWidth = 80, Margin = new Thickness(0, 0, 8, 0) };
        cancel.Click += (_, _) => Close(); Place(cancel, 3);
        _ok = new Button { Content = L.T("OK"), Appearance = ControlAppearance.Primary, MinWidth = 90 };
        _ok.Click += (_, _) => { Result = _effects.ToArray(); DialogResult = true; }; Place(_ok, 4);
        Add(footer, 6);
        Content = body;
        PreviewKeyDown += (_, e) => { if (e.Key == Key.Escape) { e.Handled = true; Close(); } };
        Show(0);
    }

    private static Button Small(string text, Action click)
    {
        var button = new Button { Content = text, Margin = new Thickness(0, 0, 8, 0), Padding = new Thickness(12, 6, 12, 6) };
        button.Click += (_, _) => click(); return button;
    }
    private void Show(int select)
    {
        _current.ItemsSource = null; _current.ItemsSource = _effects.ToArray();
        _current.SelectedIndex = _effects.Count == 0 ? -1 : Math.Min(select, _effects.Count - 1); UpdateButtons();
    }
    private void UpdateButtons()
    {
        if (_ok == null) return;
        bool picked = _choices.SelectedItem != null, selected = _current.SelectedIndex >= 0;
        _replace.IsEnabled = picked && selected;
        _add.IsEnabled = picked && _effects.Count < ItemRow.MaxGemEffects;
        _remove.IsEnabled = selected && _effects.Count > 1;
        _up.IsEnabled = _current.SelectedIndex > 0;
        _ok.IsEnabled = _effects.Count is > 0 and <= ItemRow.MaxGemEffects;
    }
    private void Replace()
    {
        if (_choices.SelectedItem is not SaveEditor.GemEffect effect || _current.SelectedIndex < 0) return;
        var index = _current.SelectedIndex; _effects[index] = effect; Show(index);
    }
    private void Append()
    {
        if (_choices.SelectedItem is not SaveEditor.GemEffect effect || _effects.Count >= ItemRow.MaxGemEffects) return;
        _effects.Add(effect); Show(_effects.Count - 1);
    }
    private void Move()
    {
        var index = _current.SelectedIndex;
        if (index <= 0) return;
        (_effects[index - 1], _effects[index]) = (_effects[index], _effects[index - 1]); Show(index - 1);
    }
}
