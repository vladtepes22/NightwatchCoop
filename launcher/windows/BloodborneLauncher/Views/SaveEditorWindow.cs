using System.IO;
using System.Globalization;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using CheckBox = System.Windows.Controls.CheckBox;
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
    private sealed class LocalSource(LauncherSettings settings) : ISaveEditorSource, IDisposable
    {
        private readonly SaveEditor.Worker _worker = new(settings);
        public Task<string> Run(string command, string kind, int? slot = null, object? edit = null, string? backup = null)
            => _worker.Run(command, kind, slot, edit, backup);
        public void Dispose() => _worker.Dispose();
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
    public const int CharacterTab = 0, InventoryTab = 1, ProgressTab = 2, BackupsTab = 3;
    private static readonly string[] Stats = { "vigor", "endurance", "strength", "skill", "bloodtinge", "arcane" };
    private static string[] StatLabels => new[] { L.T("Vigor"), L.T("Endurance"), L.T("Strength"), L.T("Skill"), L.T("Bloodtinge"), L.T("Arcane") };

    private readonly ISaveEditorSource _source;
    private readonly Grid _body = new() { Margin = new Thickness(24) };
    private readonly ListBox _slots = new();
    private readonly TextBlock _slotEmpty = LauncherPage.Text(L.T("No save found yet"), "Muted");
    private readonly TextBlock _name = LauncherPage.Text("", "PageTitle");
    private readonly TextBlock _level = LauncherPage.Text("", "Muted");
    private readonly TextBlock _status = LauncherPage.Text("", "Muted");
    private readonly ProgressRing _loadRing = new() { IsIndeterminate = true, Width = 16, Height = 16, Margin = new Thickness(0, 0, 8, 0), Visibility = Visibility.Collapsed };
    private readonly Border _warning;
    private readonly Button _apply, _revert;
    private readonly ContentControl _page = new();
    private readonly Dictionary<string, EditorField> _fields = new();
    private readonly StackPanel _backupRows = new();
    private readonly TextBox _search = new();
    private readonly DispatcherTimer _searchPause = new() { Interval = TimeSpan.FromMilliseconds(180) };
    private readonly CheckBox _changedOnly = new();
    private readonly ListBox _itemList = new(), _flagList = new();
    private readonly TextBlock _itemEmpty = LauncherPage.Text(L.T("No items found"), "Muted"), _itemCount = LauncherPage.Text("", "StatLabel");
    private readonly TextBlock _flagEmpty = LauncherPage.Text(L.T("Nothing to show here"), "Muted"), _flagNote = LauncherPage.Text("", "Muted");
    private readonly List<Button> _restoreButtons = new();
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(2) };
    private readonly UIElement[] _pages;
    private readonly string[] _tabLabels = { L.T("Character"), L.T("Inventory"), L.T("Progress"), L.T("Backups") };
    private Segments _kind = null!, _tabs = null!, _itemKinds = null!, _flagKinds = null!;
    private List<ItemRow> _items = new();
    private List<FlagRow> _flags = new();
    // Items picked in the Add item dialog; written by Apply, dropped by Revert or a reload.
    private readonly List<SaveEditorAddDialog.Pick> _added = new();
    private Button _addItem = null!;
    private SaveEditor.GemEffect[]? _effectChoices;
    private SaveEditor.Snapshot? _snapshot;
    private SaveEditor.Slot? _selected;
    private bool _busy, _loading, _dirty, _running;
    // Most players use the launcher for co-op, so the party saves open first.
    private int _kindIndex = 1;
    private string Kind => _kindIndex == 1 ? "party" : "solo";

    public SaveEditorWindow(LauncherSettings settings) : this(new LocalSource(settings)) { }
    public SaveEditorWindow(ISaveEditorSource source)
    {
        _source = source;
        if (Application.Current.MainWindow is { } owner && owner != this) Owner = owner;
        Title = L.T("Save editor"); Width = 1060; Height = 780; MinWidth = 700; MinHeight = 560;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        SetResourceReference(BackgroundProperty, "DialogBackgroundBrush");
        SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
        SetResourceReference(FontFamilyProperty, "UiFont");
        UseLayoutRounding = true; Content = _body;
        SourceInitialized += (_, _) => WindowTheme.Apply(this);
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
        _kind = new Segments(new[] { L.T("Single player"), L.T("Party") }, async index =>
        {
            if (_loading || _busy || index == _kindIndex) { _kind.Select(_kindIndex); return; }
            if (!CanDiscard()) { _kind.Select(_kindIndex); return; }
            _kindIndex = index; await Guard(() => Refresh());
        }, 6, 6, true);
        _kind.Select(_kindIndex); _kind.Panel.Margin = new Thickness(0, 6, 0, 14);
        pick.Children.Add(_kind.Panel); pick.Children.Add(LauncherPage.Text(L.T("Character slots"), "GroupHeader"));
        DockPanel.SetDock(pick, Dock.Top); sidebar.Children.Add(pick);
        var refresh = LauncherPage.SmallButton(L.T("Refresh"), SymbolRegular.ArrowClockwise24,
            async (_, _) => { if (CanDiscard()) await Guard(() => Refresh(_selected?.SlotNumber)); });
        refresh.Margin = new Thickness(0, 12, 0, 0); DockPanel.SetDock(refresh, Dock.Bottom); sidebar.Children.Add(refresh);
        var slotHost = new Grid(); StyleList(_slots); slotHost.Children.Add(_slots);
        _slotEmpty.Margin = new Thickness(8, 14, 8, 0); slotHost.Children.Add(_slotEmpty);
        sidebar.Children.Add(slotHost); columns.Children.Add(sidebar);

        var detail = new Grid();
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star) })
            detail.RowDefinitions.Add(new() { Height = height });
        Place(columns, detail, 0, 2);
        var identity = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 0, 16) };
        _name.FontSize = 26; _name.TextTrimming = TextTrimming.CharacterEllipsis;
        identity.Children.Add(_name); identity.Children.Add(_level); Place(detail, identity, 0);
        _tabs = new Segments(_tabLabels, SelectTab, 6, 8); _tabs.Panel.Margin = new Thickness(0, 0, 0, 14);
        Place(detail, _tabs.Panel, 1); Place(detail, _page, 2);
        _pages = new[] { CharacterPage(), InventoryPage(), ProgressPage(), BackupsPage() }; SelectTab(CharacterTab);

        var footer = new Grid { Margin = new Thickness(0, 18, 0, 0) };
        foreach (var width in new[] { new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto, GridLength.Auto })
            footer.ColumnDefinitions.Add(new() { Width = width });
        _status.VerticalAlignment = VerticalAlignment.Center; _status.Margin = new Thickness(0, 0, 14, 0);
        _status.TextTrimming = TextTrimming.CharacterEllipsis;
        footer.Children.Add(new DockPanel { Children = { _loadRing, _status } });
        _revert = new Button { Content = L.T("Revert"), Margin = new Thickness(0, 0, 8, 0), MinWidth = 80, IsEnabled = false };
        _revert.Click += async (_, _) => await Guard(OpenSlot); Place(footer, _revert, 0, 1);
        var close = new Button { Content = L.T("Close"), Margin = new Thickness(0, 0, 8, 0), MinWidth = 80 };
        close.Click += (_, _) => Close(); Place(footer, close, 0, 2);
        _apply = new Button { Content = L.T("Apply"), Appearance = ControlAppearance.Primary, MinWidth = 110, IsEnabled = false };
        _apply.Click += async (_, _) => await Guard(Apply); Place(footer, _apply, 0, 3); Place(_body, footer, 3);

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
        Closed += (_, _) => { _timer.Stop(); (_source as IDisposable)?.Dispose(); };
    }

    public Task LoadAsync() => Guard(() => Refresh());
    public void SelectTab(int index)
    {
        if (index < 0 || index >= _pages.Length) return;
        _page.Content = _pages[index]; _tabs.Select(index);
    }
    private async Task<T> Run<T>(string command, int? slot = null, object? edit = null, string? backup = null)
        => SaveEditor.Parse<T>(await _source.Run(command, Kind, slot, edit, backup));
    private async Task Guard(Func<Task> action)
    {
        if (_busy) return;
        // Clicks wait, the page stays as it is until the new data arrives: greying it out on every slot change reads as lag.
        _busy = true; _body.IsHitTestVisible = false; _loadRing.Visibility = Visibility.Visible;
        try { await action(); }
        catch (Exception e) { Prompt.Tell(L.T("Save editor"), e is InvalidOperationException ? e.Message : L.T("Could not open the save editor data.")); }
        finally { _busy = false; _body.IsHitTestVisible = true; _loadRing.Visibility = Visibility.Collapsed; UpdateState(); }
    }
    private bool CanDiscard() => !_dirty || Prompt.Confirm(L.T("Save editor"), L.T("Discard unsaved changes?"), L.T("Discard"));
    // keepSlot: the slot to select again after a write or a refresh (the first one when it is gone or not given).
    private async Task Refresh(int? keepSlot = null)
    {
        _loading = true;
        try
        {
            _snapshot = null; _selected = null; _dirty = false;
            var rows = await Run<SaveEditor.Slot[]>("slots"); _slots.Items.Clear();
            foreach (var slot in rows) _slots.Items.Add(SlotCard(slot));
            _slotEmpty.Visibility = rows.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
            BuildBackups(await Run<SaveEditor.Backup[]>("backups"));
            var kept = Array.FindIndex(rows, r => r.SlotNumber == keepSlot);
            _slots.SelectedIndex = rows.Length == 0 ? -1 : Math.Max(kept, 0);
        }
        finally { _loading = false; }
        await OpenSlot(); UpdateRunning();
    }
    private async Task OpenSlot()
    {
        _snapshot = null; _dirty = false; _added.Clear();
        _selected = (_slots.SelectedItem as ListBoxItem)?.Tag as SaveEditor.Slot;
        if (_selected == null) { ClearEditors(); ShowHunter(); UpdateState(); return; }
        _loading = true;
        try
        {
            var data = await Run<SaveEditor.Snapshot>("show", _selected.SlotNumber); _snapshot = data;
            var c = data.Character; _fields["name"].Load(c.Name);
            foreach (var stat in Stats) _fields[stat].Load(c.Stats[stat].ToString(CultureInfo.InvariantCulture));
            _fields["echoes"].Load(c.Echoes.ToString(CultureInfo.InvariantCulture));
            _fields["insight"].Load(c.Insight.ToString(CultureInfo.InvariantCulture));
            _fields["ng_cycle"].Load(c.NgCycle.ToString(CultureInfo.InvariantCulture));
            _items = data.Items.Select(i => new ItemRow(i, UpdateState, row => _ = Guard(() => EditGem(row)))).ToList();
            _flags = (data.Flags ?? []).Select(f => new FlagRow(f, UpdateState)).ToList();
            _itemList.ItemsSource = null;  // no refilter of the old list on the resets below
            _search.Text = ""; _changedOnly.IsChecked = false; _itemKinds.Select(0); _flagKinds.Select(0);
            BindItems(); ShowItemCount(); BindFlags();
        }
        finally { _loading = false; }
        ShowHunter(); UpdateState();
    }
    private void ClearEditors()
    {
        _items = new(); _flags = new(); BindItems(); BindFlags(); RefreshItems();
    }
    private void ShowHunter()
    {
        bool found = _snapshot != null;
        _name.Text = found ? _snapshot!.Character.Name : L.T("No save found yet");
        _level.Text = found ? L.T("Level") + " " + _snapshot!.Character.Level : L.T("Choose a save type to find your characters.");
        for (int i = 0; i < BackupsTab; i++) _tabs.Buttons[i].IsEnabled = found;
        if (!found) SelectTab(BackupsTab);
    }
    private void UpdateRunning()
    {
        _running = _source.GameRunning; _warning.Visibility = _running ? Visibility.Visible : Visibility.Collapsed; UpdateState();
    }

    // What differs from the opened save, per tab, as readable lines (also the apply confirmation).
    private (List<string> Lines, int[] PerTab) Changes()
    {
        var lines = new List<string>(); var perTab = new int[4];
        if (_snapshot == null) return (lines, perTab);
        foreach (var field in _fields.Values.Where(f => f.Changed))
        {
            lines.Add(field.Label + ": " + field.Original + " → " + field.Value); perTab[CharacterTab]++;
        }
        foreach (var row in _items.Where(r => r.Changed))
        {
            var before = lines.Count; lines.AddRange(row.Describe());
            perTab[InventoryTab] += Math.Max(1, lines.Count - before);
        }
        foreach (var pick in _added)
        {
            lines.Add(L.T("Add item") + ": " + pick.Name + (pick.Category == 4 && pick.Count > 1 ? " × " + pick.Count : "")); perTab[InventoryTab]++;
        }
        foreach (var row in _flags.Where(f => f.Changed))
        {
            lines.Add(row.Label + ": " + (row.On ? L.T("set") : L.T("cleared"))); perTab[ProgressTab]++;
        }
        return (lines, perTab);
    }
    private void UpdateState()
    {
        if (_loading || _apply == null) return;
        var (lines, perTab) = Changes();
        _dirty = lines.Count > 0;
        bool valid = _fields.Values.All(f => f.Valid) && _items.All(r => r.CountValid);
        if (_addItem != null) _addItem.IsEnabled = !_busy && !_running && _snapshot != null;
        _apply.IsEnabled = !_busy && !_running && _dirty && valid; _revert.IsEnabled = !_busy && (_dirty || !valid);
        _status.Text = _snapshot != null && !_fields["name"].Valid ? L.T("Use a name of 1 to 16 characters.") :
            !valid && _snapshot != null ? L.T("Enter whole numbers within the allowed range.") :
            _dirty ? L.T("Unsaved changes") + ": " + lines.Count : "";
        _status.SetResourceReference(TextBlock.ForegroundProperty, _dirty ? "AccentTextFillColorPrimaryBrush" : "TextFillColorSecondaryBrush");
        for (int i = 0; i < _tabLabels.Length; i++)
            _tabs.Buttons[i].Content = _tabLabels[i] + (perTab[i] > 0 ? "  ●" : "");
        foreach (var button in _restoreButtons) button.IsEnabled = !_busy && !_running;
        if (_snapshot != null && Stats.All(s => _fields[s].Valid))
        {
            var result = Stats.Sum(s => _fields[s].Number) - 50;
            var text = L.T("Level") + " " + _snapshot.Character.Level + (result != _snapshot.Character.Level ? " → " + result : "");
            _resultLevel.Text = _level.Text = text;
        }
    }
    private async Task Apply()
    {
        if (_snapshot == null || _selected == null || !_dirty) return;
        if (_fields.Values.Any(f => !f.Valid) || _items.Any(r => !r.CountValid))
            throw new InvalidOperationException(L.T("Enter whole numbers within the allowed range."));
        var (lines, _) = Changes();
        var summary = string.Join("\n", lines.Take(14).Select(l => "• " + l));
        if (lines.Count > 14) summary += "\n" + string.Format(L.T("…and {0} more"), lines.Count - 14);
        if (_items.Any(r => r.Removed && r.IsKey)) summary += "\n\n" + L.T("Removing a key item can block story progress.");
        var items = _items.Where(r => r.Changed && (r.Removed || r.CountChanged || r.LevelChanged)).Select(r =>
        {
            var change = new Dictionary<string, object> { ["section"] = r.Item.Section, ["slot"] = r.Item.Slot };
            if (r.Removed) change["remove"] = true;
            else
            {
                if (r.CountChanged) change["count"] = r.CountNumber;
                if (r.LevelChanged) change["level"] = r.Level;
            }
            return change;
        }).ToArray();
        var gems = _items.Where(r => r.GemChanged)
            .Select(r => new { r.Item.Gem!.Handle, Effects = r.Effects.Select(e => e.Id).ToArray() }).ToArray();
        var edit = new { _snapshot.Revision, Name = _fields["name"].Value,
            Stats = Stats.ToDictionary(s => s, s => _fields[s].Number), Echoes = _fields["echoes"].Number,
            Insight = _fields["insight"].Number, NgCycle = _fields["ng_cycle"].Number, Items = items, Gems = gems,
            Add = _added.Select(a => new { a.Category, a.Number, Count = a.Category == 4 ? a.Count : (int?)null }).ToArray(),
            Flags = _flags.Where(f => f.Changed).Select(f => new { f.Flag.Id, f.On }).ToArray(),
            Repair = Array.Empty<string>() };
        if (!Prompt.Confirm(L.T("Apply these edits?"), summary + "\n\n" + L.F($"All saves related to {_snapshot.Character.Name} will be backed up first."), L.T("Apply"))) return;
        var result = await Run<SaveEditor.WriteResult>("apply", _selected.SlotNumber, edit);
        Prompt.Tell(L.T("Backup folder"), result.Backup ?? ""); await Refresh(_selected.SlotNumber);
    }
    private void AddItems()
    {
        if (_snapshot == null) return;
        var held = _items.Where(r => r.Item.Section == 0 && r.Item.Kind == "goods").Select(r => (4, r.Item.Number))
            .Concat(_added.Where(a => a.Category == 4).Select(a => (4, a.Number)));
        var dialog = new SaveEditorAddDialog((category, text) => _source.Run("catalog", Kind, edit: new { category, text }), held) { Owner = this };
        if (dialog.ShowDialog() != true) return;
        foreach (var pick in dialog.Result)
            if (!_added.Any(a => a.Category == pick.Category && a.Number == pick.Number && a.Category == 4)) _added.Add(pick);
        UpdateState();
    }
    private async Task EditGem(ItemRow row)
    {
        var first = _effectChoices == null;
        _effectChoices ??= await Run<SaveEditor.GemEffect[]>("effects");
        if (first) _ = PreloadGemIcons();
        if (_effectChoices.Length == 0) throw new InvalidOperationException(L.T("The game's gem list could not be read. Check the game folder in Settings."));
        var gem = row.Item.Gem!;
        async Task<SaveEditor.GemPreview?> Preview(int[] effects)
        {
            // Only a nicety: without it the dialog and the row keep their text, and Apply still checks the effects.
            try { return await Run<SaveEditor.GemPreview>("gem_preview", edit: new { gem.Gen, gem.Shape, Effects = effects }); }
            catch (Exception e) when (e is InvalidOperationException or IOException or JsonException) { return null; }
        }
        var dialog = new SaveEditorGemDialog(row.Title, row.Effects, _effectChoices, Preview) { Owner = this };
        if (dialog.ShowDialog() == true && dialog.Result is { } result)
        {
            row.Effects = result;
            if (await Preview(result.Select(e => e.Id).ToArray()) is { } preview) row.ShowPreview(preview);
        }
    }
    private async Task PreloadGemIcons()
    {
        try { await _source.Run("gem_icons", Kind); }
        catch (Exception e) when (e is InvalidOperationException or IOException or JsonException) { }
    }
    private async Task Restore(SaveEditor.Backup backup)
    {
        if (!CanDiscard()) return;
        if (!Prompt.Confirm(L.T("Restore backup"), L.T("Restore this backup? All current saves of this type will be backed up first."), L.T("Restore"))) return;
        var result = await Run<SaveEditor.WriteResult>("restore", backup: backup.Name);
        Prompt.Tell(L.T("Backup folder"), result.Backup ?? ""); await Refresh(_selected?.SlotNumber);
    }
}
