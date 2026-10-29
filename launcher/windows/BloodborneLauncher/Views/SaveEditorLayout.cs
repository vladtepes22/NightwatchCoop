using System.Globalization;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using BloodborneLauncher.Core;
using Button = Wpf.Ui.Controls.Button;
using CheckBox = System.Windows.Controls.CheckBox;
using TextBox = Wpf.Ui.Controls.TextBox;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views;

public sealed partial class SaveEditorWindow
{
    private readonly TextBlock _resultLevel = LauncherPage.Text("", "PageTitle");
    private static void Place(Grid grid, UIElement element, int row, int column = 0)
    {
        Grid.SetRow(element, row); Grid.SetColumn(element, column); grid.Children.Add(element);
    }
    private static Border Surface(UIElement child, double padding = 16)
    {
        var border = new Border { Child = child, Padding = new Thickness(padding), CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1) };
        border.SetResourceReference(Border.BackgroundProperty, "CardBackgroundFillColorDefaultBrush");
        border.SetResourceReference(Border.BorderBrushProperty, "CardStrokeColorDefaultBrush");
        return border;
    }
    private static ScrollViewer Scroll(UIElement child) => new() { Content = child,
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        Padding = new Thickness(0, 0, 6, 0) };

    // A row of equal buttons where one is outlined in the accent colour: tabs, filters and the save type.
    private sealed class Segments
    {
        public readonly Panel Panel;
        public readonly List<Button> Buttons = new();
        public int Selected { get; private set; } = -1;
        private readonly Action<int>? _chosen;
        // wrap: buttons keep their natural width and flow onto more lines; otherwise they share the width equally.
        public Segments(string[] labels, Action<int>? chosen, double gap = 6, double padding = 8, bool wrap = false)
        {
            _chosen = chosen; Panel = wrap ? new WrapPanel() : new Grid();
            for (int i = 0; i < labels.Length; i++)
            {
                var index = i;
                var button = new Button { Content = labels[i], Padding = new Thickness(wrap ? padding + 6 : padding, padding, wrap ? padding + 6 : padding, padding),
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    Margin = wrap ? new Thickness(0, 0, gap, gap) : new Thickness(0, 0, i == labels.Length - 1 ? 0 : gap, 0) };
                button.Click += (_, _) => { Select(index); _chosen?.Invoke(index); };
                Buttons.Add(button);
                if (Panel is Grid grid) { grid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); Place(grid, button, 0, i); }
                else Panel.Children.Add(button);
            }
        }
        public void Select(int index)
        {
            Selected = index;
            for (int i = 0; i < Buttons.Count; i++)
                Buttons[i].SetResourceReference(Control.BorderBrushProperty, i == index ? "AccentTextFillColorPrimaryBrush" : "ControlStrokeColorDefaultBrush");
        }
    }

    private static TextBlock Wrapped(string text, string style, double bottom = 0)
    {
        var block = LauncherPage.Text(text, style); block.TextWrapping = TextWrapping.Wrap; block.Margin = new Thickness(0, 0, 0, bottom); return block;
    }

    // ---- Character -------------------------------------------------------------------------------------------
    private UIElement CharacterPage()
    {
        var stack = new StackPanel();
        var name = new EditorField(L.T("Name"), null, null, UpdateState); _fields["name"] = name;
        stack.Children.Add(Surface(name));
        stack.Children.Add(LauncherPage.Text(L.T("Stats"), "GroupHeader"));
        var stats = new Grid();
        for (int i = 0; i < Stats.Length; i++)
        {
            var field = new EditorField(StatLabels[i], 1, 99, UpdateState);
            _fields[Stats[i]] = field; stats.Children.Add(field);
        }
        void Arrange()
        {
            int columns = stats.ActualWidth >= 520 ? 3 : 2;
            if (stats.ColumnDefinitions.Count == columns && stats.RowDefinitions.Count == 6 / columns) return;
            stats.ColumnDefinitions.Clear(); stats.RowDefinitions.Clear();
            for (int i = 0; i < columns; i++) stats.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            for (int i = 0; i < 6 / columns; i++) stats.RowDefinitions.Add(new() { Height = GridLength.Auto });
            for (int i = 0; i < Stats.Length; i++)
            {
                var field = _fields[Stats[i]]; Grid.SetRow(field, i / columns); Grid.SetColumn(field, i % columns);
                field.Margin = new Thickness(i % columns == 0 ? 0 : 12, 0, 0, i / columns == 6 / columns - 1 ? 0 : 10);
            }
        }
        Arrange(); stats.SizeChanged += (_, _) => Arrange();
        var group = new StackPanel(); group.Children.Add(stats);
        var result = new DockPanel { Margin = new Thickness(0, 14, 0, 0) };
        _resultLevel.TextTrimming = TextTrimming.CharacterEllipsis; result.Children.Add(_resultLevel);
        group.Children.Add(result);
        group.Children.Add(Wrapped(L.T("The level always follows the six stats, as in the game."), "StatLabel"));
        stack.Children.Add(Surface(group));
        stack.Children.Add(LauncherPage.Text(L.T("Resources"), "GroupHeader"));
        var resources = new Grid();
        for (int i = 0; i < 2; i++)
        {
            resources.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            resources.RowDefinitions.Add(new() { Height = GridLength.Auto });
        }
        void Resource(string key, string label, int max, int row, int column, int span = 1)
        {
            var field = new EditorField(label, 0, max, UpdateState) { Margin = new Thickness(column == 0 ? 0 : 12, 0, 0, row == 0 ? 10 : 0) };
            _fields[key] = field; Grid.SetColumnSpan(field, span); Place(resources, field, row, column);
        }
        Resource("echoes", L.T("Echoes"), int.MaxValue, 0, 0, 2);
        Resource("insight", L.T("Insight"), 9999, 1, 0); Resource("ng_cycle", L.T("NG cycle"), 99, 1, 1);
        stack.Children.Add(Surface(resources));
        return Scroll(stack);
    }

    // ---- Inventory -------------------------------------------------------------------------------------------
    private UIElement InventoryPage()
    {
        var dock = new DockPanel();
        var tools = new Grid { Margin = new Thickness(0, 0, 0, 10) };
        tools.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        tools.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        _search.PlaceholderText = L.T("Search items"); AutomationProperties.SetName(_search, L.T("Search items"));
        _search.Margin = new Thickness(0, 0, 12, 0); tools.Children.Add(_search);
        _changedOnly.Content = L.T("Changed only"); _changedOnly.VerticalAlignment = VerticalAlignment.Center; Place(tools, _changedOnly, 0, 1);
        _search.TextChanged += (_, _) => RefreshItems(); _changedOnly.Click += (_, _) => RefreshItems();
        DockPanel.SetDock(tools, Dock.Top); dock.Children.Add(tools);
        _itemKinds = new Segments(new[] { L.T("All items"), L.T("Goods"), L.T("Weapons"), L.T("Armor"), L.T("Accessories"), L.T("Gems"), L.T("Key items") },
            _ => RefreshItems(), 4, 5, true);
        _itemKinds.Select(0); _itemKinds.Panel.Margin = new Thickness(0, 0, 0, 10);
        DockPanel.SetDock(_itemKinds.Panel, Dock.Top); dock.Children.Add(_itemKinds.Panel);
        _itemCount.Margin = new Thickness(2, 8, 0, 0); DockPanel.SetDock(_itemCount, Dock.Bottom); dock.Children.Add(_itemCount);
        var host = new Grid();
        ConfigureList(_itemList, SaveEditorXaml.ItemTemplate());
        host.Children.Add(_itemList);
        _itemEmpty.Margin = new Thickness(12, 24, 12, 0); _itemEmpty.VerticalAlignment = VerticalAlignment.Top; host.Children.Add(_itemEmpty);
        dock.Children.Add(host); return dock;
    }
    private static void ConfigureList(ListBox list, DataTemplate template)
    {
        list.BorderThickness = new Thickness(0); list.Background = System.Windows.Media.Brushes.Transparent;
        list.ItemTemplate = template; list.ItemContainerStyle = SaveEditorXaml.RowStyle();
        ScrollViewer.SetHorizontalScrollBarVisibility(list, ScrollBarVisibility.Disabled);
        VirtualizingPanel.SetIsVirtualizing(list, true); VirtualizingPanel.SetIsVirtualizingWhenGrouping(list, true);
        VirtualizingPanel.SetScrollUnit(list, ScrollUnit.Pixel);
        list.GroupStyle.Add(new GroupStyle { HeaderTemplate = SaveEditorXaml.GroupHeaderTemplate() });
    }
    private static readonly string?[] ItemFilters = { null, "goods", "weapon", "armor", "accessory", "gem", "key" };
    private void BindItems()
    {
        _itemList.ItemsSource = null;
        var view = (ListCollectionView)CollectionViewSource.GetDefaultView(_items);
        view.GroupDescriptions.Clear(); view.SortDescriptions.Clear();
        view.GroupDescriptions.Add(new PropertyGroupDescription(nameof(ItemRow.Group)));
        view.SortDescriptions.Add(new System.ComponentModel.SortDescription(nameof(ItemRow.Order), System.ComponentModel.ListSortDirection.Ascending));
        view.SortDescriptions.Add(new System.ComponentModel.SortDescription(nameof(ItemRow.Title), System.ComponentModel.ListSortDirection.Ascending));
        view.Filter = o => o is ItemRow row && ItemVisible(row);
        _itemList.ItemsSource = view;
    }
    private bool ItemVisible(ItemRow row)
    {
        var filter = ItemFilters[Math.Max(0, _itemKinds.Selected)];
        if (filter == "key" ? !row.IsKey : filter != null && (row.IsKey || row.Kind != filter)) return false;
        if (_changedOnly.IsChecked == true && !row.Changed) return false;
        var query = _search.Text.Trim();
        return query.Length == 0 || row.Title.Contains(query, StringComparison.CurrentCultureIgnoreCase) ||
            row.Item.Number.ToString(CultureInfo.InvariantCulture).Contains(query);
    }
    private void RefreshItems()
    {
        if (_itemList.ItemsSource is not ListCollectionView view) return;
        view.Refresh();
        _itemEmpty.Visibility = view.IsEmpty ? Visibility.Visible : Visibility.Collapsed;
        _itemCount.Text = _items.Count == 0 ? "" : string.Format(L.T("Showing {0} of {1}"), view.Count, _items.Count);
    }

    // ---- Progress --------------------------------------------------------------------------------------------
    private static readonly string[] FlagKinds = { "boss", "lamp", "ending" };
    private UIElement ProgressPage()
    {
        var dock = new DockPanel();
        _flagKinds = new Segments(new[] { L.T("Bosses"), L.T("Lamps"), L.T("Endings") }, _ => RefreshFlags(), 6, 8);
        _flagKinds.Select(0); _flagKinds.Panel.Margin = new Thickness(0, 0, 0, 10);
        DockPanel.SetDock(_flagKinds.Panel, Dock.Top); dock.Children.Add(_flagKinds.Panel);
        var bar = new DockPanel { Margin = new Thickness(0, 0, 0, 8) };
        var setAll = LauncherPage.SmallButton(L.T("Set all"), Wpf.Ui.Controls.SymbolRegular.CheckmarkCircle24, (_, _) => SetFlags(true));
        var clearAll = LauncherPage.SmallButton(L.T("Clear all"), Wpf.Ui.Controls.SymbolRegular.DismissCircle24, (_, _) => SetFlags(false));
        DockPanel.SetDock(clearAll, Dock.Right); DockPanel.SetDock(setAll, Dock.Right);
        bar.Children.Add(clearAll); bar.Children.Add(setAll);
        _flagNote.TextWrapping = TextWrapping.Wrap; bar.Children.Add(_flagNote);
        DockPanel.SetDock(bar, Dock.Top); dock.Children.Add(bar);
        var host = new Grid();
        ConfigureList(_flagList, SaveEditorXaml.FlagTemplate()); host.Children.Add(_flagList);
        _flagEmpty.Margin = new Thickness(12, 24, 12, 0); _flagEmpty.VerticalAlignment = VerticalAlignment.Top; host.Children.Add(_flagEmpty);
        dock.Children.Add(host); return dock;
    }
    private void BindFlags()
    {
        _flagList.ItemsSource = null;
        var view = (ListCollectionView)CollectionViewSource.GetDefaultView(_flags);
        view.GroupDescriptions.Clear(); view.SortDescriptions.Clear();
        view.GroupDescriptions.Add(new PropertyGroupDescription(nameof(FlagRow.Group)));
        view.Filter = o => o is FlagRow row && row.Flag.Kind == FlagKinds[Math.Max(0, _flagKinds.Selected)];
        _flagList.ItemsSource = view; RefreshFlags();
    }
    private void RefreshFlags()
    {
        if (_flagList.ItemsSource is not ListCollectionView view) return;
        view.Refresh();
        _flagEmpty.Visibility = view.IsEmpty ? Visibility.Visible : Visibility.Collapsed;
        _flagNote.Text = _flagKinds.Selected switch
        {
            0 => L.T("Only the kill flag changes. A boss you clear here may not fully reset its fight."),
            1 => L.T("A lit lamp can be used for warping straight away."),
            _ => L.T("Ending flags decide which endings the save has already seen.")
        };
    }
    private void SetFlags(bool on)
    {
        if (_flagList.ItemsSource is not ListCollectionView view) return;
        foreach (var row in view.Cast<FlagRow>().ToArray()) row.On = on;
    }

    // ---- Repairs ---------------------------------------------------------------------------------------------
    private UIElement RepairsPage()
    {
        _repairHost.Margin = new Thickness(0); return Scroll(_repairHost);
    }
    private void BuildRepairs(SaveEditor.Findings? findings)
    {
        _repairHost.Children.Clear(); _repairPhantom = null; _repairKeys = null;
        void Card(string title, string text, CheckBox? fix)
        {
            var body = new StackPanel(); body.Children.Add(LauncherPage.Text(title, "RowTitle")); body.Children.Add(Wrapped(text, "Muted"));
            ((TextBlock)body.Children[1]).Margin = new Thickness(0, 4, 0, fix == null ? 0 : 10);
            if (fix != null) body.Children.Add(fix);
            var card = Surface(body); card.Margin = new Thickness(0, 0, 0, 10); _repairHost.Children.Add(card);
        }
        if (findings?.Phantom == true)
        {
            _repairPhantom = new CheckBox { Content = L.T("Repair when applying") };
            _repairPhantom.Click += (_, _) => UpdateState();
            Card(L.T("Summoned-guest mark"), L.T("This character was last saved as a summoned guest. In its own world that can soft-lock the game. The repair clears the mark."), _repairPhantom);
        }
        if (findings is { DuplicateKeys.Length: > 0 })
        {
            _repairKeys = new CheckBox { Content = L.T("Repair when applying") };
            _repairKeys.Click += (_, _) => UpdateState();
            Card(L.T("Duplicate key items"), string.Format(L.T("These key items are in the inventory twice: {0}. The repair keeps one stack of each."), string.Join(", ", findings.DuplicateKeys)), _repairKeys);
        }
        if (findings?.WorldEmpty == true)
            Card(L.T("Saved in another world"), L.T("This save was last written as a guest in another player's world, so save editors that read it may fail. Use Export for save editor on the Seamless page to share a fixed copy."), null);
        if (_repairHost.Children.Count == 0)
        {
            var empty = new StackPanel { Margin = new Thickness(8, 24, 8, 24) };
            empty.Children.Add(LauncherPage.Text(L.T("Nothing to repair"), "PageTitle"));
            empty.Children.Add(Wrapped(L.T("This save passed the checks for the party problems the editor knows."), "Muted"));
            _repairHost.Children.Add(Surface(empty));
        }
    }

    // ---- Backups ---------------------------------------------------------------------------------------------
    private UIElement BackupsPage()
    {
        var dock = new DockPanel();
        var hint = LauncherPage.Text(L.T("Restoring replaces all slots of the selected save type."), "Muted");
        hint.Margin = new Thickness(0, 0, 0, 16); DockPanel.SetDock(hint, Dock.Top); dock.Children.Add(hint);
        dock.Children.Add(Scroll(_backupRows)); return dock;
    }
    private void BuildBackups(SaveEditor.Backup[] backups)
    {
        _backupRows.Children.Clear(); _restoreButtons.Clear();
        foreach (var backup in backups)
        {
            var row = new Grid(); row.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
            var info = new StackPanel { Margin = new Thickness(0, 0, 12, 0), VerticalAlignment = VerticalAlignment.Center };
            var date = backup.Name.Length >= 15 && DateTime.TryParseExact(backup.Name[..15], "yyyyMMdd-HHmmss", CultureInfo.InvariantCulture,
                DateTimeStyles.None, out var time) ? time.ToString("g", CultureInfo.CurrentCulture) : backup.Name;
            info.Children.Add(LauncherPage.Text(date, "RowTitle"));
            info.Children.Add(LauncherPage.Text(backup.Reason == "before-restore" ? L.T("Before restore") : L.T("Before edit"), "StatLabel"));
            row.Children.Add(info);
            var restore = new Button { Content = L.T("Restore"), VerticalAlignment = VerticalAlignment.Center };
            restore.Click += async (_, _) => await Guard(() => Restore(backup)); _restoreButtons.Add(restore); Place(row, restore, 0, 1);
            var card = Surface(row); card.Margin = new Thickness(0, 0, 0, 8); card.ToolTip = backup.Name; _backupRows.Children.Add(card);
        }
        if (backups.Length == 0)
        {
            var empty = new StackPanel { Margin = new Thickness(8, 24, 8, 24) };
            empty.Children.Add(LauncherPage.Text(L.T("No backups yet"), "PageTitle"));
            empty.Children.Add(LauncherPage.Text(L.T("A backup will appear here after your first change."), "Muted"));
            _backupRows.Children.Add(Surface(empty));
        }
    }

    // ---- Slot list -------------------------------------------------------------------------------------------
    private static void StyleList(ListBox list)
    {
        list.BorderThickness = new Thickness(0); list.SetResourceReference(Control.BackgroundProperty, "DialogBackgroundBrush");
        ScrollViewer.SetHorizontalScrollBarVisibility(list, ScrollBarVisibility.Disabled);
        ScrollViewer.SetVerticalScrollBarVisibility(list, ScrollBarVisibility.Auto);
        var style = new Style(typeof(ListBoxItem));
        style.Setters.Add(new Setter(Control.HorizontalContentAlignmentProperty, HorizontalAlignment.Stretch));
        style.Setters.Add(new Setter(Control.PaddingProperty, new Thickness(10)));
        style.Setters.Add(new Setter(FrameworkElement.MarginProperty, new Thickness(0, 0, 0, 8)));
        style.Setters.Add(new Setter(Control.BorderThicknessProperty, new Thickness(1)));
        style.Setters.Add(new Setter(Control.BackgroundProperty, new DynamicResourceExtension("CardBackgroundFillColorDefaultBrush")));
        style.Setters.Add(new Setter(Control.BorderBrushProperty, new DynamicResourceExtension("CardStrokeColorDefaultBrush")));
        var border = new FrameworkElementFactory(typeof(Border)); border.SetValue(Border.CornerRadiusProperty, new CornerRadius(10));
        border.SetBinding(Border.BackgroundProperty, new Binding("Background") { RelativeSource = RelativeSource.TemplatedParent });
        border.SetBinding(Border.BorderBrushProperty, new Binding("BorderBrush") { RelativeSource = RelativeSource.TemplatedParent });
        border.SetValue(Border.BorderThicknessProperty, new Thickness(1));
        var content = new FrameworkElementFactory(typeof(ContentPresenter)); content.SetValue(FrameworkElement.MarginProperty, new Thickness(10)); border.AppendChild(content);
        style.Setters.Add(new Setter(Control.TemplateProperty, new ControlTemplate(typeof(ListBoxItem)) { VisualTree = border }));
        var selected = new Trigger { Property = ListBoxItem.IsSelectedProperty, Value = true };
        selected.Setters.Add(new Setter(Control.BorderBrushProperty, new DynamicResourceExtension("AccentTextFillColorPrimaryBrush")));
        selected.Setters.Add(new Setter(Control.BackgroundProperty, new DynamicResourceExtension("SidebarItemSelectedBrush"))); style.Triggers.Add(selected);
        var hover = new Trigger { Property = UIElement.IsMouseOverProperty, Value = true };
        hover.Setters.Add(new Setter(Control.BackgroundProperty, new DynamicResourceExtension("SidebarItemHoverBrush"))); style.Triggers.Add(hover);
        list.ItemContainerStyle = style;
    }
    private static ListBoxItem SlotCard(SaveEditor.Slot slot)
    {
        var row = new Grid(); row.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        var info = new StackPanel(); info.Children.Add(LauncherPage.Text(slot.Name, "RowTitle"));
        info.Children.Add(LauncherPage.Text(L.T("Level") + " " + slot.Level, "StatLabel"));
        info.Children.Add(LauncherPage.Text(slot.Echoes.ToString("N0") + " " + L.T("Echoes"), "StatLabel")); Place(row, info, 0, 0);
        var card = new ListBoxItem { Content = row, Tag = slot, ToolTip = slot.Name };
        AutomationProperties.SetName(card, slot.Name + ", " + L.T("Level") + " " + slot.Level + ", " + slot.Echoes + " " + L.T("Echoes"));
        return card;
    }

    private sealed class EditorField : StackPanel
    {
        private readonly TextBox _box = new();
        private readonly TextBlock _dot = LauncherPage.Text("●", "StatLabel");
        private readonly int? _min, _max;
        private string _original = "";
        public string Value => _box.Text;
        public int Number => int.Parse(Value, CultureInfo.InvariantCulture);
        public string Label { get; }
        public string Original => _original;
        public bool Valid => _min.HasValue ? int.TryParse(Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) && n >= _min && n <= _max
            : Value.Length is > 0 and <= 16 && !Value.Any(char.IsControl) && !(Value.Length > 0 && char.IsHighSurrogate(Value[^1]));
        public bool Changed => _min.HasValue && int.TryParse(Value, out var value) && int.TryParse(_original, out var original) ? value != original : Value != _original;
        public void Load(string value) { _original = value; _box.Text = value; Paint(); }
        public EditorField(string label, int? min, int? max, Action changed)
        {
            _min = min; _max = max; Label = label;
            var heading = new DockPanel { Margin = new Thickness(0, 0, 0, 5) };
            _dot.SetResourceReference(TextBlock.ForegroundProperty, "AccentTextFillColorPrimaryBrush"); _dot.Margin = new Thickness(8, 0, 0, 0);
            DockPanel.SetDock(_dot, Dock.Right); heading.Children.Add(_dot); heading.Children.Add(LauncherPage.Text(label, "StatLabel")); Children.Add(heading);
            AutomationProperties.SetName(_box, label);
            var grid = new Grid(); grid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            _box.MinWidth = 36; _box.Padding = new Thickness(8, 6, 8, 6); grid.Children.Add(_box);
            if (min.HasValue)
            {
                for (int i = 0; i < 2; i++)
                {
                    int delta = i == 0 ? -1 : 1; grid.ColumnDefinitions.Add(new() { Width = new GridLength(28) });
                    var step = new Button { Content = i == 0 ? "−" : "+", Padding = new Thickness(0), Margin = new Thickness(3, 0, 0, 0),
                        HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch, MinHeight = 32 };
                    AutomationProperties.SetName(step, (i == 0 ? L.T("Decrease") : L.T("Increase")) + " " + label);
                    step.Click += (_, _) => Step(delta); Place(grid, step, 0, i + 1);
                }
                _box.ToolTip = min + "–" + max;
            }
            else _box.MaxLength = 16;
            Children.Add(grid);
            _box.TextChanged += (_, _) => { Paint(); changed(); };
            _box.PreviewKeyDown += (_, e) =>
            {
                if (e.Key == Key.Enter) { Paint(); changed(); _box.MoveFocus(new TraversalRequest(FocusNavigationDirection.Next)); e.Handled = true; }
                else if (min.HasValue && e.Key is Key.Up or Key.Down) { Step(e.Key == Key.Up ? 1 : -1); e.Handled = true; }
            };
        }
        private void Step(int delta)
        {
            if (int.TryParse(Value, out var value)) _box.Text = Math.Clamp((long)value + delta, _min!.Value, _max!.Value).ToString(CultureInfo.InvariantCulture);
        }
        private void Paint()
        {
            _dot.Visibility = Changed ? Visibility.Visible : Visibility.Hidden;
            _box.SetResourceReference(Control.BorderBrushProperty, !Valid ? "DangerTextBrush" : Changed ? "AccentTextFillColorPrimaryBrush" : "ControlStrokeColorDefaultBrush");
        }
    }
}
