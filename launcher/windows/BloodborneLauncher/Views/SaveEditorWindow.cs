using System.Globalization;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views;

// The gallery supplies JSON fixtures through this boundary; it never starts Python or opens saves.
public interface ISaveEditorSource
{
    Task<string> Run(string command, string kind, int? slot = null, object? edit = null, string? backup = null);
    bool GameRunning { get; }
}

public sealed partial class SaveEditorWindow : Window
{
    private sealed class LocalSource(LauncherSettings settings) : ISaveEditorSource
    {
        public async Task<string> Run(string command, string kind, int? slot = null, object? edit = null, string? backup = null)
            => (await SaveEditor.Run<JsonElement>(settings, command, kind, slot, edit, backup)).GetRawText();
        public bool GameRunning
        {
            get
            {
                var games = ProcessTree.GamesIn(Paths.EngineDir);
                var running = games.Length > 0;
                foreach (var game in games) game.Dispose();
                return Workspace.Current.Game.Running || running;
            }
        }
    }
    private readonly ISaveEditorSource _source;
    private readonly Grid _body = new() { Margin = new Thickness(24) };
    private readonly ComboBox _kind = new();
    private readonly ListBox _slots = new();
    private readonly TextBlock _slotEmpty = LauncherPage.Text(L.T("No save found yet"), "Muted");
    private readonly ContentControl _portrait = new();
    private readonly TextBlock _name = LauncherPage.Text("", "PageTitle");
    private readonly TextBlock _level = LauncherPage.Text("", "Muted");
    private readonly TextBlock _status = LauncherPage.Text("", "Muted");
    private readonly Border _warning;
    private readonly Button _apply;
    private readonly ContentControl _page = new();
    private readonly List<Button> _tabs = new();
    private readonly Dictionary<string, EditorField> _fields = new();
    private readonly Dictionary<SaveEditor.Item, EditorField> _counts = new();
    private readonly StackPanel _inventory = new(), _backupRows = new();
    private readonly TextBox _search = new();
    private readonly ComboBox _filter = new();
    private readonly List<Button> _restoreButtons = new();
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(2) };
    private SaveEditor.Snapshot? _snapshot;
    private SaveEditor.Slot? _selected;
    private bool _busy, _loading, _dirty, _running;
    private int _kindIndex, _tabIndex;
    private string Kind => _kindIndex == 1 ? "party" : "solo";
    private static readonly string[] Stats = { "vigor", "endurance", "strength", "skill", "bloodtinge", "arcane" };
    private readonly UIElement[] _pages;

    public SaveEditorWindow(LauncherSettings settings) : this(new LocalSource(settings)) { }
    public SaveEditorWindow(ISaveEditorSource source)
    {
        _source = source;
        if (Application.Current.MainWindow is { } owner && owner != this) Owner = owner;
        Title = L.T("Save editor"); Width = 1000; Height = 760; MinWidth = 700; MinHeight = 540;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        SetResourceReference(BackgroundProperty, "DialogBackgroundBrush");
        SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
        SetResourceReference(FontFamilyProperty, "UiFont");
        UseLayoutRounding = true; Content = _body;
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto })
            _body.RowDefinitions.Add(new() { Height = height });
        var heading = new StackPanel { Margin = new Thickness(0, 0, 0, 18) };
        heading.Children.Add(LauncherPage.Text(L.T("Save editor"), "PageTitle"));
        heading.Children.Add(LauncherPage.Text(L.T("Saves are backed up before every change. Close the game first."), "Muted"));
        _body.Children.Add(heading);
        _warning = Surface(LauncherPage.Text(L.T("Close the game before changing saves."), "Muted"), 12);
        _warning.SetResourceReference(Border.BorderBrushProperty, "WarningTextBrush");
        _warning.Margin = new Thickness(0, 0, 0, 14); _warning.Visibility = Visibility.Collapsed;
        Place(_body, _warning, 1);
        var columns = new Grid();
        foreach (var width in new[] { new GridLength(210), new GridLength(20), new GridLength(1, GridUnitType.Star) })
            columns.ColumnDefinitions.Add(new() { Width = width });
        Place(_body, columns, 2);
        _body.SizeChanged += (_, _) => columns.ColumnDefinitions[0].Width = new GridLength(_body.ActualWidth < 802 ? 184 : 210);
        var sidebar = new DockPanel();
        var pick = new StackPanel { Margin = new Thickness(0, 0, 0, 14) };
        pick.Children.Add(LauncherPage.Text(L.T("Save type"), "StatLabel"));
        _kind.Margin = new Thickness(0, 6, 0, 10);
        _kind.Items.Add(L.T("Single player")); _kind.Items.Add(L.T("Party")); _kind.SelectedIndex = 0;
        pick.Children.Add(_kind); pick.Children.Add(LauncherPage.Text(L.T("Character slots"), "GroupHeader"));
        DockPanel.SetDock(pick, Dock.Top); sidebar.Children.Add(pick);
        var refresh = LauncherPage.SmallButton(L.T("Refresh"), SymbolRegular.ArrowClockwise24,
            async (_, _) => { if (CanDiscard()) await Guard(Refresh); });
        refresh.Margin = new Thickness(0, 12, 0, 0); DockPanel.SetDock(refresh, Dock.Bottom); sidebar.Children.Add(refresh);
        var slotHost = new Grid(); StyleList(_slots); slotHost.Children.Add(_slots);
        _slotEmpty.Margin = new Thickness(8, 14, 8, 0); slotHost.Children.Add(_slotEmpty);
        sidebar.Children.Add(slotHost); columns.Children.Add(sidebar);
        var detail = new Grid();
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star) })
            detail.RowDefinitions.Add(new() { Height = height });
        Place(columns, detail, 0, 2);
        var hero = new Grid { Margin = new Thickness(0, 0, 0, 18) };
        hero.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        hero.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        _portrait.Margin = new Thickness(0, 0, 16, 0); hero.Children.Add(_portrait);
        var identity = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        _name.FontSize = 26; _name.TextTrimming = TextTrimming.CharacterEllipsis;
        identity.Children.Add(_name); identity.Children.Add(_level); Place(hero, identity, 0, 1); detail.Children.Add(hero);
        var tabs = new Grid { Margin = new Thickness(0, 0, 0, 16) };
        string[] labels = { L.T("Character"), L.T("Inventory"), L.T("Backups") };
        for (int i = 0; i < labels.Length; i++)
        {
            var index = i; tabs.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            var button = new Button { Content = labels[i], Padding = new Thickness(8), HorizontalAlignment = HorizontalAlignment.Stretch,
                Margin = new Thickness(0, 0, i == 2 ? 0 : 6, 0) };
            button.Click += (_, _) => SelectTab(index); _tabs.Add(button); Place(tabs, button, 0, i);
        }
        Place(detail, tabs, 1); Place(detail, _page, 2);
        _pages = new[] { CharacterPage(), InventoryPage(), BackupsPage() }; SelectTab(0);
        var footer = new Grid { Margin = new Thickness(0, 18, 0, 0) };
        foreach (var width in new[] { new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto })
            footer.ColumnDefinitions.Add(new() { Width = width });
        _status.VerticalAlignment = VerticalAlignment.Center; _status.Margin = new Thickness(0, 0, 14, 0); footer.Children.Add(_status);
        var close = new Button { Content = L.T("Close"), Margin = new Thickness(0, 0, 8, 0), MinWidth = 80 };
        close.Click += (_, _) => Close(); Place(footer, close, 0, 1);
        _apply = new Button { Content = L.T("Apply"), Appearance = ControlAppearance.Primary, MinWidth = 110, IsEnabled = false };
        _apply.Click += async (_, _) => await Guard(Apply); Place(footer, _apply, 0, 2); Place(_body, footer, 3);
        _kind.SelectionChanged += async (_, _) =>
        {
            if (_loading || _busy) return;
            if (!CanDiscard()) { _loading = true; _kind.SelectedIndex = _kindIndex; _loading = false; return; }
            _kindIndex = _kind.SelectedIndex; await Guard(Refresh);
        };
        _slots.SelectionChanged += async (_, _) =>
        {
            if (_loading || _busy) return;
            if (!CanDiscard())
            {
                _loading = true;
                _slots.SelectedItem = _slots.Items.Cast<ListBoxItem>().FirstOrDefault(i => Equals(i.Tag, _selected));
                _loading = false; return;
            }
            await Guard(OpenSlot);
        };
        PreviewKeyDown += (_, e) => { if (e.Key == Key.Escape) { e.Handled = true; Close(); } };
        Closing += (_, e) => { if (_busy || !CanDiscard()) e.Cancel = true; };
        _timer.Tick += (_, _) => UpdateRunning();
        Loaded += async (_, _) => { await LoadAsync(); _timer.Start(); };
        Closed += (_, _) => _timer.Stop();
    }

    public Task LoadAsync() => Guard(Refresh);
    public void SelectTab(int index)
    {
        if (index < 0 || index >= _pages.Length) return;
        _tabIndex = index; _page.Content = _pages[index];
        for (int i = 0; i < _tabs.Count; i++) _tabs[i].SetResourceReference(Control.BorderBrushProperty,
            i == index ? "AccentTextFillColorPrimaryBrush" : "ControlStrokeColorDefaultBrush");
    }
    private async Task<T> Run<T>(string command, int? slot = null, object? edit = null, string? backup = null)
        => SaveEditor.Parse<T>(await _source.Run(command, Kind, slot, edit, backup));
    private async Task Guard(Func<Task> action)
    {
        if (_busy) return;
        _busy = true; _body.IsEnabled = false;
        try { await action(); }
        catch (Exception e) { Prompt.Tell(L.T("Save editor"), e is InvalidOperationException ? e.Message : L.T("Could not open the save editor data.")); }
        finally { _busy = false; _body.IsEnabled = true; UpdateState(); }
    }
    private bool CanDiscard() => !_dirty || Prompt.Confirm(L.T("Save editor"), L.T("Discard unsaved changes?"), L.T("Discard"));
    private async Task Refresh()
    {
        _loading = true;
        try
        {
            _snapshot = null; _selected = null; _dirty = false; _counts.Clear(); _inventory.Children.Clear();
            var rows = await Run<SaveEditor.Slot[]>("slots"); _slots.Items.Clear();
            foreach (var slot in rows) _slots.Items.Add(SlotCard(slot));
            _slotEmpty.Visibility = rows.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
            BuildBackups(await Run<SaveEditor.Backup[]>("backups")); _slots.SelectedIndex = rows.Length > 0 ? 0 : -1;
        }
        finally { _loading = false; }
        await OpenSlot(); UpdateRunning();
    }
    private async Task OpenSlot()
    {
        _snapshot = null; _dirty = false;
        _selected = (_slots.SelectedItem as ListBoxItem)?.Tag as SaveEditor.Slot;
        if (_selected == null) { ShowHunter(); UpdateState(); return; }
        _loading = true;
        try
        {
            var data = await Run<SaveEditor.Snapshot>("show", _selected.SlotNumber); _snapshot = data;
            var c = data.Character; _fields["name"].Load(c.Name);
            foreach (var stat in Stats) _fields[stat].Load(c.Stats[stat].ToString(CultureInfo.InvariantCulture));
            _fields["echoes"].Load(c.Echoes.ToString(CultureInfo.InvariantCulture));
            _fields["insight"].Load(c.Insight.ToString(CultureInfo.InvariantCulture));
            _fields["ng_cycle"].Load(c.NgCycle.ToString(CultureInfo.InvariantCulture)); _counts.Clear();
            foreach (var item in data.Items)
            {
                var field = new EditorField(L.T("Count"), 1, int.MaxValue, UpdateState);
                field.Load(item.Count.ToString(CultureInfo.InvariantCulture)); _counts[item] = field;
            }
            _search.Text = ""; _filter.SelectedIndex = 0; FilterItems();
        }
        finally { _loading = false; }
        ShowHunter(); UpdateState();
    }
    private void ShowHunter()
    {
        bool found = _snapshot != null;
        _name.Text = found ? _snapshot!.Character.Name : L.T("No save found yet");
        _level.Text = found ? L.T("Level") + " " + _snapshot!.Character.Level : L.T("Choose a save type to find your characters.");
        _portrait.Content = Emblem(found ? _snapshot!.Character.Name : "", _selected?.SlotNumber ?? 0, 72);
        _tabs[0].IsEnabled = found; _tabs[1].IsEnabled = found;
        if (!found) SelectTab(2);
    }
    private void UpdateRunning()
    {
        _running = _source.GameRunning; _warning.Visibility = _running ? Visibility.Visible : Visibility.Collapsed; UpdateState();
    }
    private void UpdateState()
    {
        if (_loading || _apply == null) return;
        _dirty = _snapshot != null && (_fields.Values.Any(f => f.Changed) || _counts.Values.Any(f => f.Changed));
        bool valid = _fields.Values.All(f => f.Valid) && _counts.Values.All(f => f.Valid);
        _apply.IsEnabled = !_busy && !_running && _dirty && valid;
        _status.Text = _snapshot != null && !_fields["name"].Valid ? L.T("Use a name of 1 to 16 characters.") :
            !valid && _snapshot != null ? L.T("Enter whole numbers within the allowed range.") :
            _dirty ? L.T("Unsaved changes") : L.T("Saves are backed up before every change.");
        _status.SetResourceReference(TextBlock.ForegroundProperty, _dirty ? "AccentTextFillColorPrimaryBrush" : "TextFillColorSecondaryBrush");
        foreach (var button in _restoreButtons) button.IsEnabled = !_busy && !_running;
        if (_snapshot != null && Stats.All(s => _fields[s].Valid))
            _resultLevel.Text = _level.Text = L.T("Level") + " " + (Stats.Sum(s => _fields[s].Number) - 50);
    }
    private async Task Apply()
    {
        if (_snapshot == null || _selected == null || !_dirty) return;
        if (_fields.Values.Any(f => !f.Valid) || _counts.Values.Any(f => !f.Valid))
            throw new InvalidOperationException(L.T("Enter whole numbers within the allowed range."));
        var edit = new { _snapshot.Revision, Name = _fields["name"].Value,
            Stats = Stats.ToDictionary(s => s, s => _fields[s].Number), Echoes = _fields["echoes"].Number,
            Insight = _fields["insight"].Number, NgCycle = _fields["ng_cycle"].Number,
            Items = _snapshot.Items.Select(i => new { i.Section, i.Slot, Count = _counts[i].Number }).ToArray() };
        if (!Prompt.Confirm(L.T("Save editor"), L.T("Apply these edits? All current saves of this type will be backed up first."), L.T("Apply"))) return;
        var result = await Run<SaveEditor.WriteResult>("apply", _selected.SlotNumber, edit);
        Prompt.Tell(L.T("Backup folder"), result.Backup ?? ""); await Refresh();
    }
    private async Task Restore(SaveEditor.Backup backup)
    {
        if (!CanDiscard()) return;
        if (!Prompt.Confirm(L.T("Restore backup"), L.T("Restore this backup? All current saves of this type will be backed up first."), L.T("Restore"))) return;
        var result = await Run<SaveEditor.WriteResult>("restore", backup: backup.Name);
        Prompt.Tell(L.T("Backup folder"), result.Backup ?? ""); await Refresh();
    }
}
