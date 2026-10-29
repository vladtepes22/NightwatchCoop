using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using BloodborneLauncher.Core;
using Button = Wpf.Ui.Controls.Button;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views;

public sealed class SaveEditorWindow : Window
{
    private readonly LauncherSettings _settings;
    private readonly ComboBox _kind = new() { MinWidth = 140 };
    private readonly ComboBox _slots = new() { MinWidth = 280 };
    private readonly ComboBox _backups = new() { MinWidth = 280 };
    private readonly Dictionary<string, TextBox> _fields = new();
    private readonly DataGrid _items = new() { AutoGenerateColumns = false, CanUserAddRows = false,
        CanUserDeleteRows = false, Height = 250, SelectionMode = DataGridSelectionMode.Single };
    private readonly StackPanel _body = new() { Margin = new Thickness(22) };
    private readonly TextBlock _level = new();
    private SaveEditor.Snapshot? _snapshot;
    private int? _slot;
    private string Kind => _kind.SelectedIndex == 1 ? "party" : "solo";
    private static readonly string[] Stats = { "vigor", "endurance", "strength", "skill", "bloodtinge", "arcane" };

    public SaveEditorWindow(LauncherSettings settings)
    {
        _settings = settings;
        Owner = Application.Current.MainWindow;
        Title = L.T("Save editor");
        Width = 850; Height = 760; MinWidth = 650; MinHeight = 500;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        SetResourceReference(BackgroundProperty, "DialogBackgroundBrush");
        SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
        SetResourceReference(FontFamilyProperty, "UiFont");
        UseLayoutRounding = true;
        Content = new ScrollViewer { Content = _body, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        _body.Children.Add(LauncherPage.Text(L.T("Save editor"), "PageTitle"));
        _body.Children.Add(LauncherPage.Text(L.T("Saves are backed up before every change. Close the game first."), "Muted"));
        _kind.Items.Add(L.T("Single player")); _kind.Items.Add(L.T("Party")); _kind.SelectedIndex = 0;
        var pickers = new WrapPanel { Margin = new Thickness(0, 12, 0, 12) };
        pickers.Children.Add(_kind); pickers.Children.Add(_slots);
        pickers.Children.Add(Action(L.T("Refresh"), Refresh));
        _body.Children.Add(pickers);
        var fields = new WrapPanel();
        void Field(string key, string label)
        {
            var panel = new StackPanel { Width = 180, Margin = new Thickness(0, 0, 12, 8) };
            panel.Children.Add(LauncherPage.Text(label, "StatLabel"));
            var box = new TextBox(); _fields[key] = box; panel.Children.Add(box); fields.Children.Add(panel);
        }
        Field("name", L.T("Name"));
        Field("vigor", L.T("Vigor")); Field("endurance", L.T("Endurance"));
        Field("strength", L.T("Strength")); Field("skill", L.T("Skill"));
        Field("bloodtinge", L.T("Bloodtinge")); Field("arcane", L.T("Arcane"));
        Field("echoes", L.T("Echoes")); Field("insight", L.T("Insight")); Field("ng_cycle", L.T("NG cycle"));
        _body.Children.Add(fields); _body.Children.Add(_level);
        _body.Children.Add(LauncherPage.Text(L.T("Items"), "StatLabel"));
        void Column(string title, string property, bool readOnly = true) => _items.Columns.Add(new DataGridTextColumn
        {
            Header = title, Binding = new Binding(property) { UpdateSourceTrigger = UpdateSourceTrigger.LostFocus },
            IsReadOnly = readOnly, Width = new DataGridLength(1, DataGridLengthUnitType.Star)
        });
        Column(L.T("Name"), "Name"); Column(L.T("Item ID"), "Number");
        Column(L.T("Count"), "Count", false); Column(L.T("Level"), "Level");
        _body.Children.Add(_items);
        _body.Children.Add(Action(L.T("Apply"), Apply));
        _body.Children.Add(LauncherPage.Text(L.T("Restore backup"), "StatLabel"));
        _body.Children.Add(LauncherPage.Text(L.T("Restoring replaces all slots of the selected save type."), "Muted"));
        _body.Children.Add(_backups);
        _body.Children.Add(Action(L.T("Restore"), Restore));
        var close = new Button { Content = L.T("Close"), Margin = new Thickness(0, 10, 0, 0) };
        close.Click += (_, _) => Close(); _body.Children.Add(close);
        _kind.SelectionChanged += async (_, _) => await Guard(Refresh);
        _slots.SelectionChanged += async (_, _) => await Guard(OpenSlot);
        Loaded += async (_, _) => await Guard(Refresh);
    }

    private Button Action(string label, Func<Task> action)
    {
        var button = new Button { Content = label, Margin = new Thickness(0, 6, 0, 6) };
        button.Click += async (_, _) => await Guard(action);
        return button;
    }

    private async Task Guard(Func<Task> action)
    {
        if (!_body.IsEnabled) return;
        _body.IsEnabled = false;
        try { await action(); }
        catch (Exception e) { Prompt.Tell(L.T("Save editor"), e is InvalidOperationException ? e.Message : L.T("Could not open the save editor data.")); }
        finally { _body.IsEnabled = true; }
    }

    private async Task Refresh()
    {
        _snapshot = null; _slot = null; _items.ItemsSource = null; _level.Text = "";
        foreach (var field in _fields.Values) field.Text = "";
        var rows = await SaveEditor.Run<SaveEditor.Slot[]>(_settings, "slots", Kind);
        _slots.ItemsSource = rows;
        _backups.ItemsSource = await SaveEditor.Run<SaveEditor.Backup[]>(_settings, "backups", Kind);
        _slots.SelectedIndex = rows.Length > 0 ? 0 : -1;
        await OpenSlot();
    }

    private async Task OpenSlot()
    {
        _snapshot = null; _slot = null;
        if (_slots.SelectedItem is not SaveEditor.Slot slot) return;
        var data = await SaveEditor.Run<SaveEditor.Snapshot>(_settings, "show", Kind, slot.SlotNumber);
        _snapshot = data; _slot = slot.SlotNumber;
        var c = data.Character;
        _fields["name"].Text = c.Name;
        foreach (var stat in Stats) _fields[stat].Text = c.Stats[stat].ToString(CultureInfo.InvariantCulture);
        _fields["echoes"].Text = c.Echoes.ToString(CultureInfo.InvariantCulture);
        _fields["insight"].Text = c.Insight.ToString(CultureInfo.InvariantCulture);
        _fields["ng_cycle"].Text = c.NgCycle.ToString(CultureInfo.InvariantCulture);
        _level.Text = L.T("Level") + ": " + c.Level;
        _items.ItemsSource = data.Items;
    }

    private async Task Apply()
    {
        if (_snapshot == null || _slot == null) return;
        _items.CommitEdit(DataGridEditingUnit.Cell, true);
        _items.CommitEdit(DataGridEditingUnit.Row, true);
        if (HasErrors(_items)) throw new InvalidOperationException(L.T("Enter whole numbers within the allowed range."));
        int Number(string key) => int.TryParse(_fields[key].Text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n)
            ? n : throw new InvalidOperationException(L.T("Enter whole numbers within the allowed range."));
        var edit = new { _snapshot.Revision, Name = _fields["name"].Text,
            Stats = Stats.ToDictionary(s => s, Number), Echoes = Number("echoes"), Insight = Number("insight"),
            NgCycle = Number("ng_cycle"), Items = _snapshot.Items.Select(i => new { i.Section, i.Slot, i.Count }).ToArray() };
        if (!Prompt.Confirm(L.T("Save editor"), L.T("Apply these edits? All current saves of this type will be backed up first."), L.T("Apply"))) return;
        var result = await SaveEditor.Run<SaveEditor.WriteResult>(_settings, "apply", Kind, _slot, edit);
        Prompt.Tell(L.T("Backup folder"), result.Backup ?? "");
        await Refresh();
    }

    private static bool HasErrors(DependencyObject node)
    {
        if (Validation.GetHasError(node)) return true;
        for (var i = 0; i < System.Windows.Media.VisualTreeHelper.GetChildrenCount(node); i++)
            if (HasErrors(System.Windows.Media.VisualTreeHelper.GetChild(node, i))) return true;
        return false;
    }

    private async Task Restore()
    {
        if (_backups.SelectedItem is not SaveEditor.Backup backup) return;
        if (!Prompt.Confirm(L.T("Restore backup"), L.T("Restore this backup? All current saves of this type will be backed up first."), L.T("Restore"))) return;
        var result = await SaveEditor.Run<SaveEditor.WriteResult>(_settings, "restore", Kind, backup: backup.Name);
        Prompt.Tell(L.T("Backup folder"), result.Backup ?? "");
        await Refresh();
    }
}
