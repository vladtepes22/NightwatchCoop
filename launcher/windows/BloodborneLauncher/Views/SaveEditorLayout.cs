using System.Globalization;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using BloodborneLauncher.Core;
using Button = Wpf.Ui.Controls.Button;
using TextBox = Wpf.Ui.Controls.TextBox;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views;

public sealed partial class SaveEditorWindow
{
    private readonly TextBlock _resultLevel = LauncherPage.Text("", "RowTitle");
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
    private UIElement CharacterPage()
    {
        var stack = new StackPanel();
        var name = new EditorField(L.T("Name"), null, null, UpdateState); _fields["name"] = name;
        stack.Children.Add(Surface(name));
        stack.Children.Add(LauncherPage.Text(L.T("Stats"), "GroupHeader"));
        var stats = new Grid();
        for (int i = 0; i < 2; i++) stats.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        for (int i = 0; i < 3; i++) stats.RowDefinitions.Add(new() { Height = GridLength.Auto });
        string[] labels = { L.T("Vigor"), L.T("Endurance"), L.T("Strength"), L.T("Skill"), L.T("Bloodtinge"), L.T("Arcane") };
        for (int i = 0; i < Stats.Length; i++)
        {
            var field = new EditorField(labels[i], 1, 99, UpdateState) { Margin = new Thickness(i % 2 == 0 ? 0 : 12, 0, 0, 10) };
            _fields[Stats[i]] = field; Place(stats, field, i / 2, i % 2);
        }
        stats.SizeChanged += (_, _) =>
        {
            int columns = stats.ActualWidth >= 520 ? 3 : 2;
            if (stats.ColumnDefinitions.Count == columns) return;
            stats.ColumnDefinitions.Clear(); stats.RowDefinitions.Clear();
            for (int i = 0; i < columns; i++) stats.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            for (int i = 0; i < 6 / columns; i++) stats.RowDefinitions.Add(new() { Height = GridLength.Auto });
            for (int i = 0; i < Stats.Length; i++)
            {
                var field = _fields[Stats[i]]; Grid.SetRow(field, i / columns); Grid.SetColumn(field, i % columns);
                field.Margin = new Thickness(i % columns == 0 ? 0 : 12, 0, 0, 10);
            }
        };
        var group = new StackPanel(); group.Children.Add(stats); group.Children.Add(_resultLevel);
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
        stack.Children.Add(Surface(resources)); return Scroll(stack);
    }
    private static readonly string[] Kinds = { "", "goods", "weapon", "armor", "accessory", "gem" };
    private UIElement InventoryPage()
    {
        var dock = new DockPanel();
        var tools = new Grid { Margin = new Thickness(0, 0, 0, 12) };
        tools.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        tools.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        _search.PlaceholderText = L.T("Search items"); AutomationProperties.SetName(_search, L.T("Search items"));
        _search.Margin = new Thickness(0, 0, 10, 0); tools.Children.Add(_search);
        foreach (var label in new[] { L.T("All items"), L.T("Goods"), L.T("Weapons"), L.T("Armor"), L.T("Accessories"), L.T("Gems") }) _filter.Items.Add(label);
        _filter.SelectedIndex = 0; _filter.MinWidth = 110; AutomationProperties.SetName(_filter, L.T("Item type"));
        Place(tools, _filter, 0, 1); DockPanel.SetDock(tools, Dock.Top); dock.Children.Add(tools);
        _search.TextChanged += (_, _) => FilterItems(); _filter.SelectionChanged += (_, _) => FilterItems();
        dock.Children.Add(Scroll(_inventory)); return dock;
    }
    private static string KindLabel(string kind) => kind switch
    {
        "goods" => L.T("Goods"), "weapon" => L.T("Weapons"), "armor" => L.T("Armor"),
        "accessory" => L.T("Accessories"), "gem" => L.T("Gems"), _ => L.T("Items")
    };
    private void FilterItems()
    {
        // Editors stay alive while filtering, including invalid text and unsaved changes.
        _inventory.Children.Clear();
        foreach (var field in _counts.Values)
            if (field.Parent is Panel parent) parent.Children.Remove(field);
        var query = _search.Text.Trim();
        var kind = Kinds[Math.Max(0, _filter.SelectedIndex)];
        var rows = _counts.Where(p => (kind == "" || p.Key.Kind == kind) &&
            ((p.Key.Name ?? "").Contains(query, StringComparison.CurrentCultureIgnoreCase) || p.Key.Number.ToString(CultureInfo.InvariantCulture).Contains(query)))
            .GroupBy(p => p.Key.Kind);
        foreach (var group in rows)
        {
            _inventory.Children.Add(LauncherPage.Text(KindLabel(group.Key), "GroupHeader"));
            foreach (var (item, field) in group)
            {
                var row = new Grid(); row.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
                row.ColumnDefinitions.Add(new() { Width = new GridLength(168) });
                var info = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 12, 0) };
                var title = LauncherPage.Text(item.Name ?? L.T("Unnamed item"), "RowTitle"); title.ToolTip = title.Text; info.Children.Add(title);
                var meta = L.T("Item ID") + " " + item.Number;
                if (item.Level.HasValue) meta += " · " + L.T("Level") + " " + item.Level;
                info.Children.Add(LauncherPage.Text(meta, "StatLabel")); row.Children.Add(info); Place(row, field, 0, 1);
                var card = Surface(row, 12); card.Margin = new Thickness(0, 0, 0, 6); _inventory.Children.Add(card);
            }
        }
        if (_inventory.Children.Count == 0) _inventory.Children.Add(LauncherPage.Text(L.T("No items found"), "Muted"));
    }
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
        border.SetBinding(Border.BackgroundProperty, new System.Windows.Data.Binding("Background") { RelativeSource = System.Windows.Data.RelativeSource.TemplatedParent });
        border.SetBinding(Border.BorderBrushProperty, new System.Windows.Data.Binding("BorderBrush") { RelativeSource = System.Windows.Data.RelativeSource.TemplatedParent });
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
        public bool Valid => _min.HasValue ? int.TryParse(Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) && n >= _min && n <= _max
            : Value.Length is > 0 and <= 16 && !Value.Any(char.IsControl) && !(Value.Length > 0 && char.IsHighSurrogate(Value[^1]));
        public bool Changed => _min.HasValue && int.TryParse(Value, out var value) && int.TryParse(_original, out var original) ? value != original : Value != _original;
        public void Load(string value) { _original = value; _box.Text = value; Paint(); }
        public EditorField(string label, int? min, int? max, Action changed)
        {
            _min = min; _max = max;
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
