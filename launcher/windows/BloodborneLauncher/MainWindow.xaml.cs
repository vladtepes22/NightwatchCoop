using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using BloodborneLauncher.Core;
using BloodborneLauncher.Views;
using Wpf.Ui.Controls;
using MessageBoxButton = System.Windows.MessageBoxButton;
using MessageBoxResult = System.Windows.MessageBoxResult;

namespace BloodborneLauncher
{
    public partial class MainWindow
    {
        private readonly ObservableCollection<LogItem> _log = new();
        private readonly Dictionary<Type, LauncherPage> _pages = new();
        private bool _logExpanded;
        private bool _closing;

        public static MainWindow? Instance => Application.Current.MainWindow as MainWindow;

        public MainWindow()
        {
            InitializeComponent();

            LogList.ItemsSource = _log;
            var ws = Workspace.Current;
            ws.Logged += OnLogged;
            ws.Saved += () => SavedText.Text = L.F($"Saved {DateTime.Now:HH:mm:ss}");
            ws.GameStateChanged += UpdatePlayButton;
            ws.GameStateChanged += UpdateDiscord;
            ws.GameCrashed += OnGameCrashed;
            ws.Initialise();
            // A guest the host refused for its start-up rules takes the host's values and restarts the game to join
            // (SeamlessSettings.TakeHostStartupRules), whichever page is open.
            var hostRules = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            hostRules.Tick += async (_, _) =>
            {
                if (_restarting || !ws.Game.Running ||
                    !SeamlessSettings.TakeHostStartupRules(ws.Settings, ws.Ini, SeamlessSession.Read(ws.Settings))) return;
                ws.Log(L.T("Took the host's start-up rules: restarting the game to join."), LogLevel.Step);
                _restarting = true;
                try
                {
                    await PlayOrStopAsync();
                    if (!ws.Game.Running) await PlayOrStopAsync();
                }
                finally { _restarting = false; }
            };
            hostRules.Start();
            // The settings are read by Initialise: the saved window placement comes after it.
            RestoreWindowPlacement(ws.Settings);
            KeyDown += Window_KeyDown;
            PreviewMouseDown += Window_PreviewMouseDown;
            Deactivated += (_, _) => SearchResults.Visibility = Visibility.Collapsed;

            var args = Environment.GetCommandLineArgs();
            if (Array.IndexOf(args, "--test-prompt") is var shot and >= 0 && shot + 1 < args.Length)
            {
                Prompt.SnapshotPath = args[shot + 1];
                ShowActivated = false;
                ShowInTaskbar = false;
                WindowStartupLocation = WindowStartupLocation.Manual;
                Left = -32000;
                Top = -32000;
            }
            // --play (Bloodborne.exe): the game straight away with the saved settings, the launcher
            // out of sight meanwhile (minimized, then hidden: its prompts show on their own, in the
            // middle of the screen, and it has no taskbar button to close the game by).
            _directPlay = args.Contains("--play");
            if (_directPlay)
                WindowState = WindowState.Minimized;
            ApplyGlass();
            LauncherTheme.Apply(ws.Settings, glass: true);
            Art.Show(LauncherTheme.Eldritch(ws.Settings));
            Art.Stage = ArtStage;
            // The art moves only while the launcher is in front: a game or anything else gets the GPU.
            void Wake() => Art.Awake(IsActive && WindowState != WindowState.Minimized);
            Activated += (_, _) => Wake();
            Deactivated += (_, _) => Wake();
            StateChanged += (_, _) => Wake();
            ApplyIcon();

            var version = typeof(MainWindow).Assembly.GetName().Version;
            VersionText.Text = version == null ? "" : Updater.CurrentLabel;

            ws.UpdateStateChanged += () =>
            {
                TitleUpdate.Visibility = ws.Update != null ? Visibility.Visible : Visibility.Collapsed;
                if (ws.Update != null) TitleUpdateText.Text = L.F($"Update {ws.Update.Tag}");
            };

            Loaded += async (_, _) =>
            {
                Updater.CleanUp();
                // Running a beta means choosing beta: kept after updating to a stable release.
                if (Updater.CurrentBeta > 0 && ws.Settings.Str("update_channel").Length == 0)
                {
                    ws.Settings.Set("update_channel", "beta");
                    ws.SaveSoon();
                }
                if (ws.Settings.Bool("check_updates"))
                    _ = ws.CheckForUpdatesAsync(quiet: true);
                await Task.Yield();
                PlayItem.IsChecked = true;
                UpdatePlayButton();
                // --test-trophy: a sample notification, to check how it looks over the game.
                if (Environment.GetCommandLineArgs().Contains("--test-trophy"))
                    TrophyToast.Announce(TrophyTracker.ById[14]);
                // --test-prompt <png>: a picture of a sample dialog over the launcher, drawn off-screen.
                // --test-import: the import dialog with a long pasted code instead.
                if (Prompt.SnapshotPath != null && Environment.GetCommandLineArgs().Contains("--test-import"))
                    _ = Dispatcher.BeginInvoke(() => Prompt.Ask(L.T("Import settings"), L.F($"Paste the code someone shared (it starts with {SharedSettings.CodePrefix})."),
                            new Prompt.Field(L.T("Code"), SharedSettings.ToCode(SharedSettings.Take(ws.Ini, ws.Settings)), "", Multiline: true)),
                        System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                else if (Prompt.SnapshotPath != null)
                    _ = Dispatcher.BeginInvoke(() => Prompt.Confirm(L.T("Change language"), L.T("Restart the launcher to apply the language."), L.T("Restart now")),
                        System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                ws.Log(Paths.IsPortDir(Paths.PortDir)
                        ? L.F($"Nightwatch Co-op ready. Engine: {Paths.PortDir}")
                        : L.T("Nightwatch Co-op ready. Choose the engine folder in Settings."),
                    Paths.IsPortDir(Paths.PortDir) ? LogLevel.Good : LogLevel.Warn);
                // Once: the settings that suit this PC, shown before anything changes.
                if (_directPlay)
                    _ = Dispatcher.BeginInvoke(async () => await PlayDirectAsync(), System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                // The setup guide: with --setup, or once on a first start whose game folder is not
                // ready (a game already set up never sees it). The settings for this PC follow it.
                else if (Prompt.SnapshotPath == null && (Environment.GetCommandLineArgs().Contains("--setup") ||
                         (!ws.Settings.Bool("setup_shown") && Paths.IsPortDir(Paths.PortDir) &&
                          await Task.Run(() => GameCheck.Inspect(GameEnvironment.GameDir(ws.Settings)).Health) != Health.Good)))
                    _ = Dispatcher.BeginInvoke(() => SetupGuide.Open(firstRun: true), System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                else if (!ws.Settings.Bool("hardware_offered") && Prompt.SnapshotPath == null && Paths.IsPortDir(Paths.PortDir))
                    _ = Dispatcher.BeginInvoke(() => OfferBestSettings(firstRun: true), System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                // Once, after updating from an older launcher (a new install starts on New): the
                // newer engine or the one 0.9.33 ran, the player's choice. Nothing changes unasked.
                else if (ws.Settings.Bool("hardware_offered") && !ws.Settings.Bool("engine_preset_asked") &&
                         Prompt.SnapshotPath == null && Paths.IsPortDir(Paths.PortDir))
                    _ = Dispatcher.BeginInvoke(AskEnginePreset, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
            };
            Closing += OnClosing;
            Closed += (_, _) => MapPrep.Stop();
        }

        private bool _directPlay, _directKeepOpen;
        private bool _restarting;  // the start-up rules restart: the game stops on purpose and starts again

        /// <summary>--play: starts the game; the launcher closes when it ends normally, and opens when
        /// the start did not happen or the game crashed (the crash dialog first).</summary>
        private async Task PlayDirectAsync()
        {
            var ws = Workspace.Current;
            Hide();
            await PlayOrStopAsync();
            if (!ws.Game.Running)
            {
                _directPlay = false;
                ShowLauncher();
                return;
            }
            ws.GameCrashed += (_, _) => _directKeepOpen = true;
            ws.GameStateChanged += () =>
            {
                if (!_directPlay || ws.Game.Running || _restarting) return;
                _directPlay = false;
                if (_directKeepOpen)
                    ShowLauncher();
                else if (MapPrep.State == MapPrep.Phase.Preparing)
                {
                    // Out of sight until the map is made, then gone, as after any --play.
                    void Finished()
                    {
                        if (MapPrep.State == MapPrep.Phase.Preparing) return;
                        MapPrep.Changed -= Finished;
                        Close();
                    }
                    MapPrep.Changed += Finished;
                }
                else
                    Close();
            };
        }

        private void ShowLauncher()
        {
            Show();
            WindowState = WindowState.Normal;
            Activate();
        }

        /// <summary>Shows what would change to suit this PC, and applies it on Apply.</summary>
        /// <summary>Graphics › Engine preset, asked once: New or Legacy (0.9.33). Closed without a
        /// choice, it asks again at the next start.</summary>
        private void AskEnginePreset()
        {
            var ws = Workspace.Current;
            var choice = Prompt.Choose(L.T("New engine or the 0.9.33 one?"),
                L.T("This update brings a newer engine: a faster draw thread, quicker shader compiling and more VRAM for textures. " +
                    "Most players run faster with it. Legacy keeps the engine as 0.9.33 ran it.\n\n" +
                    "You can change it any time under Graphics > Engine preset."),
                L.T("Stay on Legacy"), L.T("Use New (recommended)"));
            if (choice < 0) return;
            EnginePreset.Apply(ws.Settings, choice == 1 ? EnginePreset.New : EnginePreset.Legacy);
            ws.Settings.Set("engine_preset_asked", true);
            ws.SaveSoon();
            ws.Log(choice == 1 ? L.T("Engine preset: New") : L.T("Engine preset: Legacy (0.9.33)"), LogLevel.Good);
        }

        public void OfferBestSettings(bool firstRun)
        {
            var ws = Workspace.Current;
            ws.Settings.Set("hardware_offered", true);
            // A new install starts on the New engine preset: nothing to ask it later.
            if (firstRun) ws.Settings.Set("engine_preset_asked", true);
            ws.SaveSoon();
            var hw = Hardware.Detect();
            var changes = hw.Recommend(ws.Settings, ws.Ini);
            if (changes.Count == 0)
            {
                if (!firstRun) Prompt.Tell(L.T("Best settings for this PC"), L.F($"{hw.Summary}\n\nYour settings already suit this PC."));
                return;
            }
            var list = string.Join("\n", changes.Select(c => $"{c.Label}: {c.From} → {c.To}"));
            if (!Prompt.Confirm(firstRun ? L.T("Settings for this PC") : L.T("Best settings for this PC"),
                    L.F($"{hw.Summary}\n\n{list}\n\nYou can change any of them later."), L.T("Apply")))
                return;
            foreach (var change in changes) change.Apply();
            ws.SaveSoon();
            ws.Log(L.T("Settings for this PC applied: ") + string.Join(", ", changes.Select(c => $"{c.Label.ToLowerInvariant()} {c.To}")) + ".", LogLevel.Good);
            ReloadPages();
        }

        /// <summary>Builds the pages again after settings changed underneath them, staying on the current one.</summary>
        public void ReloadPages()
        {
            var current = PageFrame.Content as LauncherPage;
            _pages.Clear();
            _searchPagesReady = null; // made again for the search when its box is next focused
            if (current == null) return;
            var page = _pages[current.GetType()] = (LauncherPage)Activator.CreateInstance(current.GetType())!;
            PageFrame.Navigate(page);
            page.OnShown();
        }

        /// <summary>The game ended with an error by itself: what happened, safe mode, a report to copy.</summary>
        private void OnGameCrashed(int code, SessionStats session)
        {
            var ws = Workspace.Current;
            try { LogCopy.SaveCrashReport(CrashReport.Build(ws.Settings, ws.Ini, code)); }
            catch (Exception e) { ws.Log(L.T("Could not keep the crash report: ") + e.Message, LogLevel.Warn); }
            var choice = Prompt.Choose(L.T("The game crashed"),
                L.F($"{CrashReport.Explain(code)} It ran for {SessionStats.Duration(session.Length)}.\n\n") +
                L.T("Safe mode starts the next run with the experimental engine options and mods off. ") +
                L.T("Copy report puts the details and the end of the log on the clipboard, for a bug report."),
                L.T("Close"), L.T("Copy report"), L.T("Use safe mode"));
            if (choice == 1)
            {
                try
                {
                    if (ClipboardText.Set(CrashReport.Build(ws.Settings, ws.Ini, code)))
                        ws.Log(L.T("Crash report copied to the clipboard."), LogLevel.Good);
                    else
                        ws.Log(L.T("Could not copy the report: another program is using the clipboard."), LogLevel.Warn);
                }
                catch (Exception e)
                {
                    ws.Log(L.T("Could not copy the report: ") + e.Message, LogLevel.Warn);
                }
            }
            else if (choice == 2)
            {
                ws.Settings.Set("safe_mode_next", true);
                ws.SaveSoon();
                ws.Log(L.T("The next start uses safe mode (once)."), LogLevel.Step);
            }
        }

        /// <summary>
        /// Modern layout: glass panes over a picture. Glass.xaml recolours the shared brushes;
        /// ApplyBackdrop puts the picture behind them.
        /// </summary>
        private void ApplyGlass()
        {
            Application.Current.Resources.MergedDictionaries.Add(new ResourceDictionary
            {
                Source = new Uri("pack://application:,,,/Themes/Glass.xaml", UriKind.Absolute),
            });
            ApplyBackdrop();
        }

        /// <summary>
        /// The background (Launcher › Appearance, live): the dump's sce_sys\pic1.png
        /// (or pic0, else the bundled art), the player's own picture, or none; blurred strongly,
        /// lightly or not at all, under a dark wash tinted by the theme.
        /// </summary>
        public void ApplyBackdrop()
        {
            var s = Workspace.Current.Settings;
            var shade = LauncherTheme.ShadeColour(s);
            var wash = new LinearGradientBrush(new GradientStopCollection
            {
                new(LauncherTheme.WithAlpha(shade, 0x59), 0),
                new(LauncherTheme.WithAlpha(shade, 0x80), 0.5),
                new(LauncherTheme.WithAlpha(shade, 0xB3), 1),
            }, 90);
            wash.Freeze(); // never changes: frozen brushes skip change tracking
            BackdropShade.Fill = wash;
            var mode = LauncherTheme.Eldritch(s) ? "none" : s.Str("ui_background");
            var blurSetting = s.Str("ui_background_blur");
            Art.Blur(blurSetting == "off" ? 0 : blurSetting == "light" ? 6 : 16);
            if (mode == "none")
            {
                _backdropLoad++; // a picture still loading is not shown
                BackdropImage.Visibility = Visibility.Collapsed;
                BackdropShade.Visibility = Visibility.Collapsed;
                return;
            }
            var blur = s.Str("ui_background_blur");
            var radius = blur == "off" ? 0 : blur == "light" ? 14 : 48;
            BackdropImage.Effect = radius == 0 ? null : new System.Windows.Media.Effects.BlurEffect
            {
                Radius = radius,
                KernelType = System.Windows.Media.Effects.KernelType.Gaussian,
                RenderingBias = System.Windows.Media.Effects.RenderingBias.Performance,
            };
            BackdropImage.Margin = new Thickness(radius == 0 ? 0 : -60);
            RenderOptions.SetBitmapScalingMode(BackdropImage, radius == 0 ? BitmapScalingMode.HighQuality : BitmapScalingMode.LowQuality);
            BackdropShade.Visibility = Visibility.Visible;
            _ = LoadBackdropAsync(mode, s.Str("ui_background_image"), GameEnvironment.GameDir(s),
                radius == 0 ? 2560 : radius < 20 ? 1600 : 960); // blurred: less is enough
        }

        private int _backdropLoad;

        /// <summary>Finds and decodes the background picture on a worker thread (the game folder may be
        /// on a slow or sleeping disk, and a big PNG takes a while to decode), then shows it.</summary>
        private async Task LoadBackdropAsync(string mode, string own, string game, int width)
        {
            var load = ++_backdropLoad;
            System.Windows.Media.Imaging.BitmapImage? image;
            try
            {
                image = await Task.Run(() =>
                {
                    string? art = mode == "custom" && System.IO.File.Exists(own)
                        ? own
                        : new[] { "pic1.png", "pic0.png" }
                            .Select(n => System.IO.Path.Combine(game, "sce_sys", n))
                            .FirstOrDefault(System.IO.File.Exists);
                    // No picture of our own: without the game's art the background stays plain.
                    if (art == null) return null;
                    var bitmap = new System.Windows.Media.Imaging.BitmapImage();
                    bitmap.BeginInit();
                    bitmap.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad;
                    bitmap.DecodePixelWidth = width;
                    bitmap.UriSource = new Uri(art, UriKind.Absolute);
                    bitmap.EndInit();
                    bitmap.Freeze(); // decoded here, usable on the UI thread
                    return bitmap;
                });
            }
            catch
            {
                image = null;
            }
            if (load != _backdropLoad) return; // a newer choice is loading
            // A picture that won't load: the glass sits on the plain dark background.
            BackdropImage.Source = image;
            BackdropImage.Visibility = image != null ? Visibility.Visible : Visibility.Collapsed;
        }

        /// <summary>The theme, again (Launcher › Appearance › Theme, live).</summary>
        public void ApplyTheme()
        {
            LauncherTheme.Apply(Workspace.Current.Settings, glass: true);
            ApplyBackdrop();
            Art.Show(LauncherTheme.Eldritch(Workspace.Current.Settings));
        }

        /// <summary>Launcher › App icon: this window's, and the game window's from its next start.</summary>
        public void ApplyIcon()
        {
            var choice = Workspace.Current.Settings.Str("app_icon");
            AppIcon.Apply(this, choice);
            if (Paths.DetectPortDir() is { } package)
                AppIcon.SyncGameWindow(package, choice);
        }

        /// <summary>A wider resize border than FluentWindow's 4px, and the top strip handed back
        /// to Windows as a real caption.</summary>
        protected override void SetWindowChrome()
        {
            base.SetWindowChrome();
            if (System.Windows.Shell.WindowChrome.GetWindowChrome(this) is { } chrome)
            {
                if (ResizeMode != ResizeMode.NoResize)
                    chrome.ResizeBorderThickness = new Thickness(8);
                chrome.CaptionHeight = 34;
            }
        }

        private DateTime? _gameStartedUtc;

        /// <summary>Discord status: shown while the game runs (Core/DiscordPresence).</summary>
        public void UpdateDiscord()
        {
            var running = Workspace.Current.Game.Running;
            if (running && _gameStartedUtc == null) _gameStartedUtc = DateTime.UtcNow;
            if (!running) _gameStartedUtc = null;
            DiscordPresence.Update(Workspace.Current.Settings, running, _gameStartedUtc);
        }

        /// <summary>The size and position the window had when it was last closed (settings.json:
        /// window_width, window_height, window_x, window_y, window_maximized), else the default size.
        /// Either way it is made to fit the screen: 1040 x 780 does not fit a 1080p screen at 150% or
        /// 200% Windows scaling. A saved position no screen shows any more (a monitor unplugged) is
        /// dropped and the window opens centred.</summary>
        private void RestoreWindowPlacement(LauncherSettings settings)
        {
            try
            {
                var work = SystemParameters.WorkArea;
                double width = Width, height = Height;
                int savedWidth = settings.Int("window_width"), savedHeight = settings.Int("window_height");
                if (savedWidth >= 400 && savedHeight >= 300)
                {
                    width = savedWidth;
                    height = savedHeight;
                }
                MinWidth = Math.Min(MinWidth, work.Width - 16);
                MinHeight = Math.Min(MinHeight, work.Height - 16);
                width = Math.Max(MinWidth, Math.Min(width, work.Width - 16));
                height = Math.Max(MinHeight, Math.Min(height, work.Height - 16));
                Width = width;
                Height = height;
                if (settings.Str("window_x").Length > 0 && settings.Str("window_y").Length > 0)
                {
                    double x = settings.Int("window_x"), y = settings.Int("window_y");
                    double left = SystemParameters.VirtualScreenLeft, top = SystemParameters.VirtualScreenTop;
                    double right = left + SystemParameters.VirtualScreenWidth, bottom = top + SystemParameters.VirtualScreenHeight;
                    // The title bar must be on a screen, or the window could not be moved back.
                    if (x + 120 >= left && x + 120 < right && y + 20 >= top && y + 20 < bottom)
                    {
                        WindowStartupLocation = WindowStartupLocation.Manual;
                        Left = Math.Max(left, Math.Min(x, right - width));
                        Top = Math.Max(top, Math.Min(y, bottom - height));
                    }
                }
                if (settings.Bool("window_maximized"))
                    WindowState = WindowState.Maximized;
            }
            catch (Exception)
            {
                // The default size, centred.
            }
        }

        /// <summary>Called as the window closes. A maximised window keeps the size and position it
        /// had before (RestoreBounds); a minimised one (the --play start) saves nothing.</summary>
        private void SaveWindowPlacement()
        {
            try
            {
                if (_directPlay || WindowState == WindowState.Minimized || !IsVisible) return;
                var bounds = WindowState == WindowState.Normal ? new Rect(Left, Top, ActualWidth, ActualHeight) : RestoreBounds;
                if (bounds.IsEmpty || double.IsNaN(bounds.Left) || double.IsNaN(bounds.Top) || bounds.Width < 1 || bounds.Height < 1) return;
                var settings = Workspace.Current.Settings;
                settings.Set("window_maximized", WindowState == WindowState.Maximized);
                settings.Set("window_width", (int)Math.Round(bounds.Width));
                settings.Set("window_height", (int)Math.Round(bounds.Height));
                settings.Set("window_x", (int)Math.Round(bounds.Left));
                settings.Set("window_y", (int)Math.Round(bounds.Top));
            }
            catch (Exception)
            {
                // The window closes without remembering where it was.
            }
        }

        private async void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
        {
            SaveWindowPlacement();
            Workspace.Current.SaveNow();
            DiscordPresence.Stop();
            // Closing stops making the map (it starts over at the next start).
            if (!_closing && MapPrep.State == MapPrep.Phase.Preparing &&
                System.Windows.MessageBox.Show(this,
                    L.F($"The map is still being made ({MapPrep.Percent}%). Closing the launcher stops it; it starts over the next time. Close anyway?"),
                    "Nightwatch Co-op", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            {
                e.Cancel = true;
                return;
            }
            if (_closing || !Workspace.Current.Game.Running)
                return;

            var answer = System.Windows.MessageBox.Show(this,
                L.T("The game is still running. Stop it and close the launcher?"),
                "Nightwatch Co-op", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
            if (answer == MessageBoxResult.Cancel)
            {
                e.Cancel = true;
                return;
            }
            if (answer == MessageBoxResult.Yes)
            {
                e.Cancel = true;
                _closing = true;
                await Workspace.Current.StopGameAsync();
                // Queued, not called: a game that exited while the question was open makes the stop
                // finish at once, and Close() inside this closing event throws.
                _ = Dispatcher.BeginInvoke(Close);
            }
        }

        // ---------------------------------------------------------------- window buttons

        private void CloseWindow_Click(object sender, RoutedEventArgs e) => Close();
        private void MinimizeWindow_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
        private void MaximizeWindow_Click(object sender, RoutedEventArgs e) =>
            WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

        /// <summary>Three quick clicks on the Co-op tab show the watcher; one click still opens the tab.</summary>
        private void CoopItem_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ClickCount == 3 && LauncherTheme.Eldritch(Workspace.Current.Settings)) ShowWatcher();
        }

        /// <summary>The easter egg: a huge eye in a wound of flesh, ringed by tentacles, opens over the launcher (Eldritch theme only),
        /// fixes its pupil on you and shuts again (under three seconds). Clicks go on through it. Plain frame swaps
        /// and one slow zoom on a dark scrim: no effects, nothing re-rendered underneath.</summary>
        private void ShowWatcher()
        {
            if (Watcher.Visibility == Visibility.Visible) return;
            var frames = new[] { "closed", "half", "open", "focus" }
                .Select(f => (ImageSource?)EldritchArt.Picture($"watcher-{f}.png", 1536)).ToArray();
            if (frames.Any(f => f == null)) return; // not in this build
            static DoubleAnimationUsingKeyFrames Keys(params (double Value, int Ms)[] keys)
            {
                var animation = new DoubleAnimationUsingKeyFrames();
                foreach (var (value, ms) in keys)
                    animation.KeyFrames.Add(new EasingDoubleKeyFrame(value, TimeSpan.FromMilliseconds(ms), new SineEase { EasingMode = EasingMode.EaseInOut }));
                return animation;
            }
            WatcherEye.Width = Math.Min(ActualWidth * 0.8, 1100);
            WatcherEye.Source = frames[0];
            var fade = Keys((0, 0), (1, 256), (1, 2496), (0, 2816));
            fade.Completed += (_, _) =>
            {
                Watcher.Visibility = Visibility.Collapsed;
                foreach (var (target, property) in new (System.Windows.Media.Animation.IAnimatable, DependencyProperty)[]
                         { (Watcher, OpacityProperty), (WatcherText, OpacityProperty), (WatcherZoom, ScaleTransform.ScaleXProperty),
                           (WatcherZoom, ScaleTransform.ScaleYProperty), (WatcherEye, System.Windows.Controls.Image.SourceProperty) })
                    target.BeginAnimation(property, null);
            };
            Watcher.Visibility = Visibility.Visible;
            Watcher.BeginAnimation(OpacityProperty, fade);
            // closed, a flicker, open, then the pupil narrows on you; it holds, half-closes, shuts.
            var lids = new ObjectAnimationUsingKeyFrames();
            foreach (var (frame, ms) in new[] { (0, 0), (1, 288), (0, 358), (1, 448), (2, 525), (3, 1216), (2, 2144), (1, 2272), (0, 2368) })
                lids.KeyFrames.Add(new DiscreteObjectKeyFrame(frames[frame], TimeSpan.FromMilliseconds(ms)));
            WatcherEye.BeginAnimation(System.Windows.Controls.Image.SourceProperty, lids);
            var zoom = Keys((0.9, 0), (1.06, 2368));
            WatcherZoom.BeginAnimation(ScaleTransform.ScaleXProperty, zoom);
            WatcherZoom.BeginAnimation(ScaleTransform.ScaleYProperty, zoom);
            WatcherText.BeginAnimation(OpacityProperty, Keys((0, 0), (0, 1216), (1, 1600), (1, 2176), (0, 2432)));
        }

        // ---------------------------------------------------------------- navigation

        private void SidebarItem_Checked(object sender, RoutedEventArgs e)
        {
            if (sender is not System.Windows.Controls.RadioButton { Tag: Type pageType } item)
                return;

            var page = PageOf(pageType);
            PageFrame.Navigate(page);
            page.OnShown();

            var name = ItemName(item);
            TitlePage.Text = string.IsNullOrEmpty(name) ? "" : $" — {name}";
        }

        /// <summary>A page, made the first time it is asked for and kept.</summary>
        private LauncherPage PageOf(Type type)
        {
            if (!_pages.TryGetValue(type, out var page))
                _pages[type] = page = (LauncherPage)Activator.CreateInstance(type)!;
            return page;
        }

        /// <summary>A tab's or sidebar entry's label: its text, or the text beside its icon.</summary>
        private static string? ItemName(System.Windows.Controls.RadioButton item) =>
            item.Content as string ??
            (item.Content as Panel)?.Children.OfType<System.Windows.Controls.TextBlock>().FirstOrDefault()?.Text;

        /// <summary>Shows a page: a tab ("seamless": Co-op, "advanced": Settings), or one of More's ("output", "mods", "engine").</summary>
        public void Show(string page)
        {
            var item = page switch
            {
                "seamless" => CoopItem,
                "advanced" => SettingsItem,
                "output" or "mods" or "engine" or "cheats" => MoreItem,
                "controls" => ControlsItem,
                _ => PlayItem,
            };
            if (item.IsChecked == true)
                SidebarItem_Checked(item, new RoutedEventArgs());
            item.IsChecked = true;
            if (item == MoreItem && _pages.TryGetValue(typeof(MorePage), out var more))
                ((MorePage)more).ShowSection(page);
        }

        private void PageFrame_Navigated(object sender, System.Windows.Navigation.NavigationEventArgs e)
        {
            while (PageFrame.CanGoBack)
                PageFrame.RemoveBackEntry();
        }

        // ---------------------------------------------------------------- settings search

        private List<SettingSearch.Hit> _hits = new();
        private int _hit;

        /// <summary>
        /// The pages this layout's tabs or sidebar lead to, in their order, each made if not shown yet
        /// (once; they are kept as when visited). More stands for its sections, each a place of its own.
        /// </summary>
        private IEnumerable<SettingSearch.Place> SearchPlaces()
        {
            foreach (var item in SettingSearch.Descendants(TabStrip).OfType<System.Windows.Controls.RadioButton>().Where(i => i.Tag is Type))
            {
                var name = ItemName(item) ?? "";
                void Open() { if (item.IsChecked != true) item.IsChecked = true; }
                // Only pages already made: MakeSearchPagesAsync makes the rest in the background and
                // searches again once they are all there.
                if (!_pages.TryGetValue((Type)item.Tag, out var made)) continue;
                if (made is MorePage more)
                {
                    foreach (var (key, label, page) in more.MadeSectionPages())
                        yield return new SettingSearch.Place($"{name} › {label}", page, () => { Open(); more.ShowSection(key); });
                }
                else
                    yield return new SettingSearch.Place(name, made, Open);
            }
        }

        private void Window_KeyDown(object sender, KeyEventArgs e)
        {
            // Bubbling, so a key the page took first (the Controls page listening for a binding) stays its own.
            if (e.Key == Key.F && Keyboard.Modifiers == ModifierKeys.Control)
            {
                SearchBox.Focus();
                SearchBox.SelectAll();
                e.Handled = true;
            }
        }

        private void Window_PreviewMouseDown(object sender, MouseButtonEventArgs e)
        {
            // A click anywhere else closes the results (the text stays; typing opens them again).
            if (SearchResults.IsVisible && e.OriginalSource is Visual source &&
                !SearchResults.IsAncestorOf(source) && !SearchArea.IsAncestorOf(source))
                SearchResults.Visibility = Visibility.Collapsed;
        }

        /// <summary>Searches 150 ms after the last key, not at every key.</summary>
        private System.Windows.Threading.DispatcherTimer? _searchDelay;

        /// <summary>The pages the search looks through, made one at a time at background priority so
        /// typing never waits for them (null until the search box is first focused).</summary>
        private Task? _searchPagesReady;

        private void SearchBox_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
        {
            SearchClear.Visibility = SearchBox.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            if (SearchBox.Text.Trim().Length == 0)
            {
                _searchDelay?.Stop();
                ShowHits();
                return;
            }
            if (_searchDelay == null)
            {
                _searchDelay = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
                _searchDelay.Tick += (_, _) =>
                {
                    _searchDelay.Stop();
                    ShowHits();
                };
            }
            _searchDelay.Stop();
            _searchDelay.Start();
        }

        private void SearchBox_GotKeyboardFocus(object sender, KeyboardFocusChangedEventArgs e)
        {
            _searchPagesReady ??= MakeSearchPagesAsync();
            ShowHits();
        }

        /// <summary>Makes every page the search looks through, yielding to input and rendering between
        /// pages: the first search then finds them ready instead of building them all in one go.</summary>
        private async Task MakeSearchPagesAsync()
        {
            var items = SettingSearch.Descendants(TabStrip)
                .OfType<System.Windows.Controls.RadioButton>()
                .Select(i => i.Tag).OfType<Type>().Distinct().ToList();
            foreach (var type in items)
            {
                await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Background);
                if (PageOf(type) is MorePage more)
                    foreach (var key in more.SectionKeys)
                    {
                        await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Background);
                        more.MakeSection(key);
                    }
            }
            // What was typed while the pages were being made, searched again over all of them.
            if (SearchBox.IsKeyboardFocused && SearchBox.Text.Trim().Length > 0 && _searchDelay?.IsEnabled != true)
                ShowHits();
        }

        private void SearchBox_LostKeyboardFocus(object sender, KeyboardFocusChangedEventArgs e) =>
            SearchResults.Visibility = Visibility.Collapsed;

        private void SearchClear_Click(object sender, RoutedEventArgs e)
        {
            _searchDelay?.Stop();
            SearchBox.Clear();
            SearchBox.Focus();
        }

        private void SearchBox_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            switch (e.Key)
            {
                case Key.Escape:
                    // Esc clears; on an empty box it hands the keyboard back to the page.
                    if (SearchBox.Text.Length > 0) SearchBox.Clear();
                    else Keyboard.Focus(this);
                    e.Handled = true;
                    break;
                case Key.Enter:
                    // Typed faster than the search delay: search now, so Enter picks for what is typed.
                    if (_searchDelay?.IsEnabled == true)
                    {
                        _searchDelay.Stop();
                        ShowHits();
                    }
                    if (_hits.Count > 0 && SearchResults.IsVisible) PickHit(_hit);
                    e.Handled = true;
                    break;
                case Key.Down or Key.Up when _hits.Count > 0:
                    _hit = (_hit + (e.Key == Key.Down ? 1 : _hits.Count - 1)) % _hits.Count;
                    MarkHit();
                    e.Handled = true;
                    break;
            }
        }

        /// <summary>Searches again for what is typed, and shows the results under the box.</summary>
        private void ShowHits()
        {
            var query = SearchBox.Text;
            if (query.Trim().Length == 0 || !SearchBox.IsKeyboardFocused)
            {
                _hits.Clear();
                SearchResults.Visibility = Visibility.Collapsed;
                return;
            }
            _hits = SettingSearch.Find(SearchPlaces(), query);
            _hit = 0;
            SearchList.Children.Clear();
            if (_hits.Count == 0)
                SearchList.Children.Add(SettingSearch.Nothing(query));
            for (var i = 0; i < _hits.Count; i++)
            {
                var index = i;
                var view = SettingSearch.View(_hits[i]);
                view.MouseEnter += (_, _) =>
                {
                    _hit = index;
                    MarkHit();
                };
                view.MouseLeftButtonUp += (_, _) => PickHit(index);
                SearchList.Children.Add(view);
            }
            MarkHit();
            SearchResults.Visibility = Visibility.Visible;
        }

        /// <summary>Shades the result Enter picks (the first, until the arrow keys or the mouse move it).</summary>
        private void MarkHit()
        {
            for (var i = 0; i < SearchList.Children.Count; i++)
                if (SearchList.Children[i] is System.Windows.Controls.Border { Tag: SettingSearch.Hit } view)
                {
                    if (i == _hit) view.SetResourceReference(System.Windows.Controls.Border.BackgroundProperty, "SidebarItemSelectedBrush");
                    else view.Background = Brushes.Transparent;
                }
        }

        /// <summary>Opens the result's page and, once it is laid out, scrolls to the setting and flashes it.</summary>
        private void PickHit(int index)
        {
            if (index < 0 || index >= _hits.Count) return;
            var hit = _hits[index];
            SearchBox.Clear();
            SearchResults.Visibility = Visibility.Collapsed;
            hit.Place.Show();
            // A page shown for the first time is navigated to, loaded and grouped into its boxes first.
            Dispatcher.BeginInvoke(() =>
            {
                if (!hit.Place.Page.Reveal(hit.Row))
                    Keyboard.Focus(this);
            }, System.Windows.Threading.DispatcherPriority.ContextIdle);
        }

        // ---------------------------------------------------------------- play

        private async void TitlePlay_Click(object sender, RoutedEventArgs e) => await PlayOrStopAsync();

        private async void TitleUpdate_Click(object sender, RoutedEventArgs e) => await InstallUpdateAsync();

        /// <summary>The log button: a small menu to copy the newest session logs (as files) or the last crash
        /// report (as text), IP addresses hidden in both.</summary>
        private void TitleLogs_Click(object sender, RoutedEventArgs e)
        {
            var menu = new System.Windows.Controls.ContextMenu
            {
                PlacementTarget = TitleLogs, Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom,
            };
            var logs = new System.Windows.Controls.MenuItem { Header = L.F($"Copy last {LogCopy.Count} logs") };
            logs.Click += (_, _) => CopyLogs();
            var crash = new System.Windows.Controls.MenuItem { Header = L.T("Copy last crash report") };
            crash.Click += (_, _) => CopyCrashReport();
            menu.Items.Add(logs);
            menu.Items.Add(crash);
            menu.IsOpen = true;
        }

        private static void CopyLogs()
        {
            var ws = Workspace.Current;
            try
            {
                switch (LogCopy.CopyLogs())
                {
                    case 0: ws.Log(L.T("No session logs yet: they are written when the game runs."), LogLevel.Warn); break;
                    case null: ws.Log(L.T("Could not copy the logs: another program is using the clipboard."), LogLevel.Warn); break;
                    case { } n: ws.Log(L.F($"{n} logs copied (IP addresses hidden): paste them into Discord."), LogLevel.Good); break;
                }
            }
            catch (Exception e)
            {
                ws.Log(L.T("Could not copy the logs: ") + e.Message, LogLevel.Warn);
            }
        }

        private static void CopyCrashReport()
        {
            var ws = Workspace.Current;
            switch (LogCopy.CopyCrashReport())
            {
                case null: ws.Log(L.T("No crash report: the game has not crashed since this was added."), LogLevel.Step); break;
                case true: ws.Log(L.T("Crash report copied to the clipboard (IP addresses hidden)."), LogLevel.Good); break;
                case false: ws.Log(L.T("Could not copy the report: another program is using the clipboard."), LogLevel.Warn); break;
            }
        }

        /// <summary>Asks, downloads the newer launcher, swaps it in and restarts.</summary>
        public async Task InstallUpdateAsync()
        {
            var ws = Workspace.Current;
            if (ws.Update is not { } release) return;
            if (await PackageCheck(() => Updater.NeedsPackageAsync(release)) is { } package)
                await InstallAsync(release, package);
        }

        /// <summary>Whether an update needs the whole package, or null (logged) when the release manifest
        /// could not be read: guessing would install a new launcher over an engine it does not match.</summary>
        private static async Task<bool?> PackageCheck(Func<Task<bool>> check)
        {
            try { return await check(); }
            catch (InvalidOperationException ex)
            {
                Workspace.Current.Log(L.T("Update failed: ") + ex.Message, LogLevel.Bad);
                return null;
            }
        }

        /// <summary>An older package's engine: download the latest package and update it in place.</summary>
        public async Task UpdateEngineAsync()
        {
            var ws = Workspace.Current;
            var release = ws.Latest ?? await Updater.LatestAsync(Updater.BetaChannel(ws.Settings));
            if (release?.ZipUrl == null)
            {
                Prompt.Tell(L.T("Update engine"), L.F($"No package is published yet. See {Updater.ReleasesPage}"));
                return;
            }
            await InstallAsync(release, package: true);
        }

        /// <summary>A release the player picked in Launcher › Updates › Version, older ones included.</summary>
        public async Task InstallVersionAsync(Updater.Release release)
        {
            if (release.IsCurrent)
            {
                Prompt.Tell(L.T("Already installed"), L.F($"You already have {Updater.CurrentLabel}."));
                return;
            }
            if (await PackageCheck(() => Updater.SwitchNeedsPackageAsync(release)) is { } package)
                await InstallAsync(release, package, chosen: true);
        }

        private bool _updating;

        private async Task InstallAsync(Updater.Release release, bool package, bool chosen = false)
        {
            if (_updating) return;
            var ws = Workspace.Current;
            if (release.FreshDownload)
            {
                // Installed by hand into a new folder: its release page says what to copy over.
                var text = release.Notes.Trim();
                if (text.Length > 1200) text = text[..1200] + "…";
                if (Prompt.Confirm(L.F($"{release.Name} is a fresh download"),
                        L.T("This version is not installed over this one: download its zip, extract it to a new folder and copy your saves and settings over (its release page says how).") +
                        (text.Length > 0 ? "\n\n" + L.T("What's new") + ":\n" + text : ""),
                        L.T("Open the download page")))
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(release.Page) { UseShellExecute = true });
                return;
            }
            if (package && ws.Game.Running)
            {
                Prompt.Tell(L.T("Close the game first"), L.T("This update replaces the engine, so the game has to be closed."));
                return;
            }
            var notes = release.Notes.Trim();
            if (notes.Length > 1200) notes = notes[..1200] + "…";
            var size = package ? release.ZipSize : release.ExeSize;
            var older = !release.IsNewerThan(Updater.Current, Updater.CurrentBeta);
            var body = L.F($"You have {Updater.CurrentLabel}. ") +
                       (package
                           ? L.F($"{(chosen ? "This version has a different engine" : "This update includes a new engine")}, so the whole package is downloaded ({size / 1048576} MB) and replaced in place.")
                           : L.F($"The launcher downloads {release.Tag} ({size / 1048576} MB), replaces itself and restarts.")) +
                       L.T(" Your settings, saves, mods and profiles stay.") +
                       (chosen && older
                           ? L.T("\n\nThis is an older version: settings it doesn't know are kept but not used, and the launcher may offer the newest version again at start.")
                           : "") +
                       (notes.Length > 0 ? $"\n\n{(chosen && older ? L.T("This version's notes") : L.T("What's new"))}:\n" + notes : "");
            var verb = chosen ? L.T("Switch") : L.T("Update");
            if (!Prompt.Confirm(L.F($"{verb} to {release.Name}?"), body, L.F($"{verb} now")))
                return;

            TitleUpdate.Visibility = Visibility.Visible;
            TitleUpdate.IsEnabled = false;
            _updating = true;
            UpdatePlayButton();
            var progress = new Progress<double>(p => TitleUpdateText.Text = L.F($"Downloading {p:P0}"));
            try
            {
                ws.SaveNow();
                var skipped = await Updater.InstallAsync(release, package, progress);
                if (skipped.Count > 0)
                    Prompt.Tell(L.T("Updated, with files left out"),
                        L.T("Windows or an antivirus would not let these through, so the old ones were kept:\n") +
                        string.Join("\n", skipped.Take(5)) +
                        L.T("\n\nThe game runs without them; online play may need ") +
                        L.F($"them. Allow the Nightwatch Co-op folder in the antivirus and update again, or take them from {Updater.ReleasesPage}"));
                Application.Current.Shutdown();
            }
            catch (Exception ex)
            {
                _updating = false;
                UpdatePlayButton();
                TitleUpdate.IsEnabled = true;
                TitleUpdateText.Text = L.F($"Update {release.Tag}");
                ws.Log(L.T("Update failed: ") + ex.GetBaseException().Message, LogLevel.Bad);
                Prompt.Tell(L.T("Update failed"), ex.GetBaseException().Message + L.F($"\n\nYou can download it by hand from {Updater.ReleasesPage}"));
            }
        }

        /// <summary>The one place the game is started from, so the title bar and the Play page agree.</summary>
        public async Task PlayOrStopAsync()
        {
            if (_updating) return;
            var ws = Workspace.Current;
            if (ws.Game.Running)
            {
                await ws.StopGameAsync();
                return;
            }

            // The launcher updated itself but the package kept an older engine: its crash fixes are
            // missing (players reported crashes long fixed). Asked at each Play until it is updated.
            if (Updater.EngineOutdated)
            {
                var choice = Prompt.Choose(L.T("Engine update needed"),
                    L.F($"This launcher is {Updater.CurrentLabel}, but the engine in this folder is older ({Updater.LocalEngine() ?? "unknown"}): ") +
                    L.T("the crash fixes since then are missing. Updating downloads the latest package and keeps your settings, saves and mods."),
                    L.T("Cancel"), L.T("Play anyway"), L.T("Update engine"));
                if (choice <= 0) return;
                if (choice == 2)
                {
                    await UpdateEngineAsync();
                    return;
                }
            }

            var s = ws.Settings;
            var ini = ws.Ini;
            if (SeamlessSettings.Validate(s) is { } partyProblem)
            {
                Prompt.Tell(L.T("Co-op"), partyProblem);
                Show("seamless");
                return;
            }
            // No public online play (no shadNet, no The Hunter's Dream): co-op is the party's own.
            ini.Set("online_enabled", "0");
            ini.Set("online_pool", "");

            // No port folder yet: look for it again, then let the player point at it right here.
            if (!GameRunner.Custom(s) && !Paths.IsPortDir(Paths.PortDir))
            {
                ws.ResolvePort();
                if (!Paths.IsPortDir(Paths.PortDir))
                {
                    if (!Prompt.Confirm(L.T("Where is the engine?"),
                            L.T("The launcher needs the engine (the folder with run.py and bin\\bb-probe.exe). ") +
                            L.T("It is normally the folder the launcher came in."), L.T("Choose folder")))
                        return;
                    var dialog = new Microsoft.Win32.OpenFolderDialog { Title = L.T("Port folder (with Bloodborne.exe and run.py)") };
                    if (dialog.ShowDialog(this) != true) return;
                    if (!Paths.IsPortDir(dialog.FolderName))
                    {
                        Prompt.Tell(L.T("Not the port folder"), L.T("That folder has no run.py with Bloodborne.exe or bin\\bb-probe.exe."));
                        return;
                    }
                    s.Set("port_dir", dialog.FolderName);
                    ws.ResolvePort();
                    ws.SaveNow();
                }
            }

            // No game folder with eboot.bin: the engine would only log "No eboot.bin". The setup
            // guide says what is missing instead.
            if (!GameRunner.Custom(s) && !System.IO.File.Exists(System.IO.Path.Combine(GameEnvironment.GameDir(s), "eboot.bin")))
            {
                if (Prompt.Choose(L.T("The game is not set up yet"),
                        L.T("The launcher has no game folder with eboot.bin in it, so the game cannot start. The setup guide shows where your Bloodborne files go and checks them."),
                        L.T("Cancel"), L.T("Open the setup guide")) == 1)
                {
                    ShowLauncher();
                    SetupGuide.Open();
                }
                return;
            }

            GameEnvironment.RegionPatchOnline = null;

            // A third-party resolution patch while the output resolution sets the game's resolution
            // and UI itself: it would keep its own shape (black bars) and stop the widescreen hooks.
            GameEnvironment.ResolutionPatchChoice = null;
            var (outW, outH) = Choices.OutputSize(ini.Get("output_res", "1920x1080"));
            // With a 1920 × 1080 output, such a patch makes the game draw at its size itself: the
            // upscaler then runs at 1:1 (DLAA whatever the preset) and in-game preset changes crashed
            // (Discord, 3440 × 1440). Its size as the output resolution keeps the shape and lets the
            // presets work: offered once per set of patches.
            if ((outW, outH) == (1920, 1080) &&
                Patches.ResolutionSetting(GameEnvironment.PatchesDir(s)) is { Count: > 0 } native &&
                native.Select(p => System.Text.RegularExpressions.Regex.Match(p.Name, @"(\d{3,5})x(\d{3,5})"))
                    .FirstOrDefault(m => m.Success) is { } size &&
                (int.Parse(size.Groups[1].Value), int.Parse(size.Groups[2].Value)) is var (patchW, patchH) &&
                (patchW, patchH) != (1920, 1080))
            {
                var names = string.Join(", ", native.Select(p => $"\"{p.Name}\""));
                if (s.Str("resolution_patch_native") != names)
                {
                    var choice = Prompt.Choose(L.F($"Use {patchW} × {patchH} as the output resolution?"),
                        names + L.F($" makes the game draw at {patchW} × {patchH} by itself, so the upscaler preset can't lower ") +
                        L.T("the render resolution (the upscaler runs as DLAA / native AA).\n\n") +
                        L.F($"Setting the output resolution to {patchW} × {patchH} instead gives the same picture and shape, ") +
                        L.T("and the presets (Quality, Balanced, Performance) work. The patch is then left out while that output is set."),
                        L.T("Cancel"), L.F($"Use {patchW} × {patchH}"), L.T("Keep the patch"));
                    if (choice <= 0) return;
                    if (choice == 1)
                    {
                        ini.Set("output_res", $"{patchW}x{patchH}");
                        ini.Save();
                        (outW, outH) = (patchW, patchH);
                        GameEnvironment.ResolutionPatchChoice = "skip";
                        ws.Log(L.F($"Output resolution {patchW} × {patchH}; {names} left out (presets work)."), LogLevel.Step);
                    }
                    else
                    {
                        s.Set("resolution_patch_native", names);
                        ws.SaveNow();
                    }
                }
            }
            if ((outW, outH) != (1920, 1080) && GameEnvironment.ResolutionPatchChoice is null &&
                Patches.ResolutionSetting(GameEnvironment.PatchesDir(s)) is { Count: > 0 } sizing)
            {
                var names = string.Join(", ", sizing.Select(p => $"\"{p.Name}\""));
                // "Always on" is remembered for this set of patches (#18); another set asks again.
                var choice = s.Str("resolution_patch_always") == names ? 1 : Prompt.Choose(L.T("Play with ") + names + "?",
                    L.F($"The output resolution ({outW} × {outH}) already sets the game's resolution and widens the picture and UI to fill the screen. ") +
                    names + L.T(" sets its own on top: the picture keeps the patch's shape, with black bars.\n\n") +
                    L.T("Leave it out? It still applies with a 1920 × 1080 output. \"Always on\" keeps it and does not ask again ") +
                    L.T("until the resolution patches switched on in Mods change."),
                    L.T("Cancel"), L.T("Keep on"), L.T("Leave out"), L.T("Always on"));
                if (choice <= 0) return;
                if (choice == 3)
                {
                    s.Set("resolution_patch_always", names);
                    ws.SaveNow();
                    choice = 1;
                }
                GameEnvironment.ResolutionPatchChoice = choice == 1 ? "keep" : "skip";
                ws.Log(choice == 1 ? L.F($"{names}: kept on with the {outW} × {outH} output.")
                    : L.F($"{names}: left out; the output resolution sets the picture."), LogLevel.Step);
            }

            // A game left running in the background (an earlier start that never ended, or another
            // launcher's) keeps its whole memory pool, and a new start fails (Windows error 1450).
            var leftovers = ProcessTree.GamesIn(Paths.EngineDir);
            if (leftovers.Length > 0)
            {
                var choice = Prompt.Choose(L.T("The game is still running in the background"),
                    L.T("bb-probe.exe from an earlier start is still running and holds the game's memory, so a new start would fail. ") +
                    L.T("Close it and start?"), L.T("Cancel"), L.T("Close it and start"));
                if (choice <= 0) return;
                foreach (var leftover in leftovers)
                {
                    try
                    {
                        leftover.Kill(entireProcessTree: true);
                        using var wait = new System.Threading.CancellationTokenSource(5000);
                        await leftover.WaitForExitAsync(wait.Token); // off the UI thread
                    }
                    catch { /* already gone */ }
                    leftover.Dispose();
                }
                ws.Log(L.T("Closed the game left running in the background."), LogLevel.Step);
            }

            if (!s.Bool("memory_check_off") && MemoryCheck.Problem(ws.Ini) is { } memory)
            {
                var choice = Prompt.Choose(L.T("Windows may run out of memory"), memory, L.T("Cancel"), L.T("Don't ask again"), L.T("Start anyway"));
                if (choice <= 0) return;
                if (choice == 1) s.Set("memory_check_off", true);
            }

            // bbport: the current profile has no saves, but saves were found elsewhere (a folder used
            // before, or SaveGuard's safety copies): asked before the game starts a new one.
            var savesRoot = GameEnvironment.SavesRoot(s);
            var savesProfile = PlayerProfiles.Current(s);
            var packageSaves = System.IO.Path.Combine(Paths.DataDir, "user");
            var lost = await System.Threading.Tasks.Task.Run(() => SaveGuard.Elsewhere(savesRoot, savesProfile, new[] { packageSaves }));
            if (lost.FirstOrDefault() is { } foundSaves)
            {
                var (start, changed) = SaveGuard.Ask(s, foundSaves, Prompt.Choose, Prompt.Tell);
                if (changed)
                {
                    ws.SaveNow();
                    ws.Log(foundSaves.Copy == null ? L.F($"Saves folder: {foundSaves.Root}")
                                                   : L.T("Saves restored from the safety copy."), LogLevel.Good);
                    ReloadPages();
                }
                if (!start) return;
            }

            if (_updating) return;
            var error = ws.StartGame();
            if (error != null)
            {
                System.Windows.MessageBox.Show(this, error, L.T("Could not start the game"), MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            // Boot splash over a minimized launcher (the direct --play launcher is already hidden).
            // If the game ends before its window shows, the launcher comes back.
            if (!_directPlay) WindowState = WindowState.Minimized;
            var splash = SplashWindow.Open(() => ws.Game.Running, s.Bool("splash_off"));
            if (splash != null)
                splash.Finished += gameEnded =>
                {
                    if (gameEnded && !_directPlay) ShowLauncher();
                };
            // The map is still being made: play now, the game says so and turns the map on by itself.
            if (MapPrep.State == MapPrep.Phase.Preparing)
                ws.Log(L.F($"Heads up: the map is still being made ({MapPrep.Percent}%). Play on: the game shows a note and turns the map on by itself when it is ready, no restart needed."), LogLevel.Warn);
            if (s.Bool("switch_to_output"))
                Show("output");
        }

        private void UpdatePlayButton()
        {
            TitlePlay.IsEnabled = !_updating;
            var running = Workspace.Current.Game.Running;
            TitlePlayText.Text = running ? L.T("Stop") : L.T("Play");
            TitlePlayIcon.Symbol = running ? SymbolRegular.Stop24 : SymbolRegular.Play24;
            TitlePlayIcon.Foreground = (Brush)FindResource(running ? "DangerTextBrush" : "AccentTextFillColorPrimaryBrush");
            TitlePlay.ToolTip = running ? L.T("Stop the game") : L.T("Start the game");
            foreach (var page in _pages.Values)
                (page as PlayPage)?.UpdateRunning();
        }

        // ---------------------------------------------------------------- log

        /// <summary>The activity log keeps the newest entries only (the game's output has its own page).</summary>
        private const int MaxLogItems = 500;

        /// <summary>The log list's own scroller (inside its template, so the list can virtualize).</summary>
        private System.Windows.Controls.ScrollViewer? LogScroller =>
            LogList.Template?.FindName("LogScroller", LogList) as System.Windows.Controls.ScrollViewer;

        private void OnLogged(string message, LogLevel level)
        {
            _log.Add(new LogItem { Time = DateTime.Now.ToString("HH:mm:ss"), Message = message, Color = BrushFor(level) });
            if (_log.Count > MaxLogItems) _log.RemoveAt(0);
            LogCount.Text = L.F($"{_log.Count} events");
            // Folded: nothing to scroll (ToggleLog scrolls to the end when it opens).
            if (_logExpanded) LogScroller?.ScrollToEnd();

            StatusText.Text = message;
            StatusText.ToolTip = message;
            StatusDot.Fill = level == LogLevel.Info ? (Brush)FindResource("StatusDotIdleBrush") : BrushFor(level);
        }

        private Brush BrushFor(LogLevel level) => (Brush)FindResource(level switch
        {
            LogLevel.Good => "GoodTextBrush",
            LogLevel.Bad => "DangerTextBrush",
            LogLevel.Warn => "WarningTextBrush",
            LogLevel.Step => "AccentTextFillColorPrimaryBrush",
            _ => "TextFillColorSecondaryBrush",
        });

        private void ClearLog_Click(object sender, RoutedEventArgs e)
        {
            _log.Clear();
            LogCount.Text = "";
            StatusText.Text = L.T("Ready");
            StatusText.ToolTip = null;
            StatusDot.Fill = (Brush)FindResource("StatusDotIdleBrush");
        }

        private void ToggleLog_Click(object sender, RoutedEventArgs e) => ToggleLog();
        private void StatusBar_Click(object sender, MouseButtonEventArgs e) => ToggleLog();

        private void ToggleLog()
        {
            _logExpanded = !_logExpanded;
            LogPanel.Visibility = _logExpanded ? Visibility.Visible : Visibility.Collapsed;
            if (_logExpanded)
                Dispatcher.BeginInvoke(() => LogScroller?.ScrollToEnd(), System.Windows.Threading.DispatcherPriority.Loaded);
        }
    }

    public class LogItem
    {
        public string Time { get; set; } = "";
        public string Message { get; set; } = "";
        public Brush Color { get; set; } = Brushes.White;
    }
}
