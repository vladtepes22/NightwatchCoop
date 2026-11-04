using System.IO;
using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>Mod folders with their load order, the port's built-in optional patches, and the player's
    /// own (third-party) eboot patches from the patches folder, each in a section of its own.</summary>
    public sealed class ModsPage : LauncherPage
    {
        private readonly StackPanel _modList = new();
        private readonly StackPanel _patchList = new();
        private readonly TextBlock _modsHint, _patchesHint;
        private List<Mods.Entry> _mods = new();
        private List<(Patches.Patch Patch, bool On)> _patches = new();
        private Dictionary<string, List<Mods.Conflict>> _clashes = new();
        private readonly System.Windows.Controls.ComboBox _modeBox = new() { Width = 220, VerticalAlignment = VerticalAlignment.Center };
        private readonly ProgressRing _modeRing = new() { IsIndeterminate = true, Width = 18, Height = 18, Margin = new Thickness(0, 0, 10, 0), Visibility = Visibility.Collapsed };
        private readonly TextBlock _modeHint;
        private bool _modeSetting;

        public ModsPage() : base(L.T("Mods & patches"),
            L.T("Extract each mod into its own folder (with dvdroot_ps4, or straight chr/, parts/ and so on). Applied when the game starts."))
        {
            Group(L.T("Mods"));
            foreach (var (label, value) in ModeChoices())
                _modeBox.Items.Add(new ComboBoxItem { Content = label, Tag = value });
            _modeBox.SelectionChanged += (_, _) => ModePicked();
            Add(Card(SymbolRegular.PuzzlePiece24, L.T("Load mods"), L.T("When two mods replace the same file, the one lower in the list wins"),
                Row(_modeRing, _modeBox), out _modeHint));
            ShowMode();
            Add(Card(SymbolRegular.Folder24, L.T("Mods folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) =>
                {
                    if (PickFolder(L.T("Mods folder"), GameEnvironment.ModsDir(S)) is { } path)
                    {
                        S.Set("mods_dir", path);
                        Ws.SaveSoon();
                        RefreshMods();
                    }
                }),
                IconButton(SymbolRegular.Open24, L.T("Open in Explorer"), (_, _) => Paths.OpenFolder(GameEnvironment.ModsDir(S))),
                IconButton(SymbolRegular.ArrowClockwise24, L.T("Refresh the list"), (_, _) => RefreshMods())), out _modsHint));
            Body.Children.Add(_modList);

            Group(L.T("Third-party patches"));
            Body.Children.Add(Text(L.T("shadPS4-format XML patches for version 01.09 that you put in the patches folder. Put each in a file of its own: Bloodborne.xml is replaced on every update."), "Faint"));
            Add(Card(SymbolRegular.Folder24, L.T("Patches folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) =>
                {
                    if (PickFolder(L.T("Patches folder"), GameEnvironment.PatchesDir(S)) is { } path)
                    {
                        S.Set("patches_dir", path);
                        Ws.SaveSoon();
                        RefreshPatches();
                    }
                }),
                IconButton(SymbolRegular.Open24, L.T("Open in Explorer"), (_, _) => Paths.OpenFolder(GameEnvironment.PatchesDir(S))),
                IconButton(SymbolRegular.ArrowClockwise24, L.T("Refresh the list"), (_, _) => RefreshPatches())), out _patchesHint));
            Body.Children.Add(_patchList);


            Ws.PortChanged += OnShown;
            SwitchChanged += ShowMode;
        }


        /// <summary>Once, the first time the Workshop opens: Nocturne, with its Nexus Mods page and a one-click switch (it was a
        /// card on the Play page; user, 4 Nov). After the page is on screen.</summary>
        private void NocturneOnce()
        {
            if (S.Bool("nocturne_workshop_seen")) return;
            S.Set("nocturne_workshop_seen", true);
            Ws.SaveSoon();
            if (CuratedMods.Mode(S) == "nocturne") return;
            Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle, async () =>
            {
                var pick = Prompt.Choose(L.T("Nocturne"),
                    L.T("A curated mod for the game. Try it here (Load mods: Nocturne only, downloaded once), or read about it on its Nexus Mods page."),
                    L.T("Not now"), L.T("Nexus Mods"), L.T("Try Nocturne"));
                if (pick == 1)
                {
                    try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(CuratedMods.NexusPage) { UseShellExecute = true }); }
                    catch (Exception) { Prompt.Tell(L.T("Nexus Mods"), CuratedMods.NexusPage); }
                }
                else if (pick == 2) await ChooseMode("nocturne");
            });
        }

        private static (string Label, string Value)[] ModeChoices() => new[]
        {
            (L.T("No mods"), "standard"), (L.T("My mods"), "my"), (L.T("Nocturne only"), "nocturne"),
        };

        /// <summary>The combo, the mod list and the lock (the game reads the mods at its start only).</summary>
        private void ShowMode()
        {
            var mode = Switching ?? CuratedMods.Mode(S);
            var busy = Switching != null;
            _modeRing.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
            _modeSetting = true;
            _modeBox.SelectedIndex = Array.FindIndex(ModeChoices(), c => c.Value == mode);
            _modeSetting = false;
            _modList.IsEnabled = mode == "my";
            var locked = Ws.Game.Running;
            _modeBox.IsEnabled = !locked && !busy;
            if (busy)
                SetHint(_modeHint, mode == "nocturne" ? DownloadText() : "");
            else
                SetHint(_modeHint, locked ? L.T("Close the game to change which mods load.") :
                    L.T("No mods: the game as it ships. My mods: the list below. Nocturne only: the curated mod, with your mods paused. Applied when the game starts."));
        }

        private async void ModePicked()
        {
            if (_modeSetting || Switching != null || _modeBox.SelectedItem is not ComboBoxItem { Tag: string mode } || mode == CuratedMods.Mode(S)) return;
            await ChooseMode(mode);
        }

        private async Task ChooseMode(string mode)
        {
            var error = await SwitchModeAsync(Ws, mode, null);
            ShowMode(); // the old mode again when it failed
            if (error is { Length: > 0 }) Prompt.Tell(L.T("Load mods"), error);
            _modsShown = null;
            RefreshMods();
        }

        /// <summary>Switches which mods load (the dropdown and the Workshop's one-time Nocturne popup). Picking Nocturne installs or updates it first, asking
        /// before the download. Returns null when switched, "" when the player declined, else the message to show; the mode is
        /// unchanged then.</summary>
        internal static async Task<string?> SwitchModeAsync(Workspace ws, string mode, IProgress<double>? progress)
        {
            if (ws.Game.Running) return L.T("Close the game to change which mods load.");
            if (Switching != null) return ""; // one switch at a time; every page already shows the one running
            Switching = mode;
            SwitchProgress = 0;
            SwitchChanged?.Invoke();
            var shared = new Progress<double>(p => { SwitchProgress = p; SwitchChanged?.Invoke(); progress?.Report(p); });
            try { return await SwitchCoreAsync(ws, mode, shared); }
            finally { Switching = null; SwitchChanged?.Invoke(); }
        }

        /// <summary>The choice being switched to (Nocturne while it downloads), shown by the Mods page, the Host tab's Mods line
        /// until the switch ends; null when none runs. SwitchChanged fires on the UI thread.</summary>
        internal static string? Switching { get; private set; }
        internal static double SwitchProgress { get; private set; }
        internal static event Action? SwitchChanged;
        internal static string DownloadText() => L.F($"Downloading Nocturne: {(int)(SwitchProgress * 100)}%");

        private static async Task<string?> SwitchCoreAsync(Workspace ws, string mode, IProgress<double> progress)
        {
            var s = ws.Settings;
            var dir = GameEnvironment.ModsDir(s);
            var before = s.Str(CuratedMods.ProfileKey);
            try
            {
                if (mode == "nocturne")
                {
                    ModManifest? manifest = null;
                    string? unreachable = null;
                    try { manifest = await CuratedMods.FetchManifestAsync(); }
                    catch (Exception e) { unreachable = e.Message; }
                    var installed = await Task.Run(() => CuratedMods.Installed(dir, manifest));
                    var newest = manifest?.Newest;
                    var want = newest != null && (installed.Kind == InstalledKind.None ||
                        (installed.Kind == InstalledKind.Known && installed.Version != newest.Version)) ? newest : null;
                    if (installed.Kind == InstalledKind.None && want == null)
                        return unreachable != null
                            ? L.F($"Nocturne is not installed and its download could not be reached: {unreachable}")
                            : L.T("Nocturne is not installed and no download is set up.");
                    if (want != null)
                    {
                        var mb = (int)Math.Ceiling(want.Size / 1e6);
                        var ok = Prompt.Confirm(L.T("Nocturne"), installed.Kind == InstalledKind.None
                            ? L.F($"Nocturne {want.Version} is downloaded once ({mb} MB) and kept in your mods folder. Continue?")
                            : L.F($"Nocturne {want.Version} is available ({mb} MB). It replaces your installed version. Continue?"), L.T("Download"));
                        if (!ok && installed.Kind == InstalledKind.None) return "";
                        if (ok)
                            await CuratedMods.InstallAsync(want, dir, progress, CancellationToken.None);
                    }
                }
                s.Set(CuratedMods.ProfileKey, mode);
                if (mode == "my") s.Set("mods_enabled", true); // the old Load mods switch: off reads as No mods (CuratedMods.Mode)
                CuratedMods.Apply(s, dir);
                ws.SaveSoon();
                return null;
            }
            catch (Exception e)
            {
                s.Set(CuratedMods.ProfileKey, before);
                return L.F($"Nocturne could not be set up: {e.Message}");
            }
        }

        public override void OnShown()
        {
            NocturneOnce();
            ShowMode();
            RefreshMods();
            RefreshPatches();
        }

        private int _modReads, _patchReads;
        private string? _modsShown, _patchesShown;

        /// <summary>Reads the mods folder on a worker thread; the list is made again only when it changed.</summary>
        private async void RefreshMods()
        {
            var read = ++_modReads;
            var dir = GameEnvironment.ModsDir(S);
            List<Mods.Entry> mods;
            Dictionary<string, List<Mods.Conflict>> clashes;
            try
            {
                var paused = CuratedMods.Mode(S) != "my";
                (mods, clashes) = await Task.Run(() =>
                {
                    var list = Mods.Load(dir);
                    return (list, paused ? new Dictionary<string, List<Mods.Conflict>>()
                        : Mods.Conflicts(dir, list.Where(m => m.Enabled).Select(m => m.Name)));
                });
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not read the mods folder: ") + e.Message, LogLevel.Warn);
                return;
            }
            if (read != _modReads) return;
            _mods = mods;
            _clashes = clashes;
            SetHint(_modsHint, _mods.Count == 0 ? L.F($"{dir} — no mods") : _mods.Count == 1 ? L.F($"{dir} — 1 mod") : L.F($"{dir} — {_mods.Count} mods"));
            if (ModsKey() == _modsShown) return;
            DrawMods();
        }

        private string ModsKey() => string.Join("|", _mods.Select(m => $"{m.Name}={m.Enabled}{ClashText(m)}"));

        /// <summary>The lines under an enabled mod that shares files with another enabled one.</summary>
        private string ClashText(Mods.Entry mod) =>
            !mod.Enabled || !_clashes.TryGetValue(mod.Name, out var list) ? "" :
                string.Join("\n", list.Select(c => L.F($"Also replaced by {c.With} ({string.Join(", ", c.Examples.Select(Path.GetFileName))}): the one lower in the load order wins")));

        /// <summary>Which mods clash after a switch was flipped.</summary>
        private async void RefreshClashes()
        {
            var read = ++_modReads;
            var dir = GameEnvironment.ModsDir(S);
            var enabled = _mods.Where(m => m.Enabled).Select(m => m.Name).ToList();
            try
            {
                var clashes = await Task.Run(() => Mods.Conflicts(dir, enabled));
                if (read != _modReads) return;
                _clashes = clashes;
                DrawMods();
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not read the mods folder: ") + e.Message, LogLevel.Warn);
            }
        }

        private void DrawMods()
        {
            _modsShown = ModsKey();
            _modList.Children.Clear();
            for (var i = 0; i < _mods.Count; i++)
            {
                var index = i;
                var mod = _mods[i];
                var toggle = new ToggleSwitch { IsChecked = mod.Enabled, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 0, 0) };
                toggle.Click += (_, _) =>
                {
                    _mods[index] = mod with { Enabled = toggle.IsChecked == true };
                    SaveMods();
                    RefreshClashes();
                };
                var up = IconButton(SymbolRegular.ArrowUp24, L.T("Load earlier"), (_, _) => Move(index, -1));
                var down = IconButton(SymbolRegular.ArrowDown24, L.T("Load later"), (_, _) => Move(index, 1));
                up.IsEnabled = i > 0;
                down.IsEnabled = i < _mods.Count - 1;

                var clash = ClashText(mod);
                var card = Card(SymbolRegular.Box24, mod.Name, L.F($"Load order {i + 1}") + (clash.Length > 0 ? "\n" + clash : ""), Row(up, down, toggle), out _);
                card.Margin = new Thickness(24, 0, 0, 4);
                _modList.Children.Add(card);
            }
        }

        private void Move(int index, int direction)
        {
            var target = index + direction;
            if (target < 0 || target >= _mods.Count) return;
            (_mods[index], _mods[target]) = (_mods[target], _mods[index]);
            SaveMods();
            DrawMods();
        }

        private void SaveMods()
        {
            try
            {
                Mods.Save(_mods);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not save mods.json: ") + e.Message, LogLevel.Bad);
            }
        }

        /// <summary>Reads the patch files on a worker thread (every XML in the folder is parsed); the
        /// list is made again only when it changed.</summary>
        private async void RefreshPatches()
        {
            var read = ++_patchReads;
            var dir = GameEnvironment.PatchesDir(S);
            List<(Patches.Patch, bool)> patches;
            try
            {
                patches = await Task.Run(() =>
                {
                    var profile = Patches.LoadProfile();
                    return Patches.Discover(dir).Select(p => (p, Patches.IsOn(p, profile))).ToList();
                });
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not read the patches folder: ") + e.Message, LogLevel.Warn);
                return;
            }
            if (read != _patchReads) return;
            _patches = patches;
            SetHint(_patchesHint, _patches.Count > 0 ? dir : L.F($"{dir} — no patches"));
            var shown = string.Join("\u0001", _patches.Select(p => $"{p.Patch.Key}|{p.Patch.Name}|{p.Patch.File}|{p.Patch.Author}|{p.Patch.Note}={p.On}"));
            if (shown == _patchesShown) return;
            _patchesShown = shown;

            _patchList.Children.Clear();
            for (var i = 0; i < _patches.Count; i++)
            {
                var index = i;
                var (patch, on) = _patches[i];
                var toggle = new ToggleSwitch { IsChecked = on, VerticalAlignment = VerticalAlignment.Center };
                toggle.Click += (_, _) =>
                {
                    _patches[index] = (patch, toggle.IsChecked == true);
                    try
                    {
                        Patches.Save(_patches);
                    }
                    catch (Exception e)
                    {
                        Ws.Log(L.T("Could not save patches.json: ") + e.Message, LogLevel.Bad);
                    }
                };
                var source = patch.File + (patch.Author.Length > 0 ? L.F($" · by {patch.Author}") : "");
                var hint = patch.Note.Length > 0 ? patch.Note + "\n\n" + source : source;
                var card = Card(SymbolRegular.DocumentText24, patch.Name, hint, toggle, out _);
                card.Margin = new Thickness(24, 0, 0, 4);
                if (patch.Note.Length > 0)
                    card.ToolTip = patch.Note;
                _patchList.Children.Add(card);
            }
        }
    }
}
