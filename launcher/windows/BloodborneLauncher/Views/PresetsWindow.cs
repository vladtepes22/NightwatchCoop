using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The graphics presets (Core/Presets.cs) in their own window, opened from the Graphics page's
    /// "Presets…" button: one row per preset (the one in use shows Selected) and "Back to my settings"
    /// while values from before the first preset are kept. A preset fills in the Graphics page's
    /// picture, effect and lighting settings, which stay editable there.
    /// </summary>
    public sealed class PresetsWindow : Window
    {
        private static Workspace Ws => Workspace.Current;

        private readonly StackPanel _rows = new();
        private bool _changed;

        /// <summary>Opens the window (modal); the pages show the new values when a preset was used.</summary>
        public static void Open()
        {
            var window = new PresetsWindow();
            window.ShowDialog();
            if (window._changed) MainWindow.Instance?.ReloadPages();
        }

        private PresetsWindow()
        {
            var owner = Application.Current.MainWindow is { IsLoaded: true, IsVisible: true } main && main.WindowState != WindowState.Minimized ? main : null;
            Owner = owner;
            WindowStartupLocation = owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen;
            TextOptions.SetTextFormattingMode(this, TextFormattingMode.Display);
            UseLayoutRounding = true;
            WindowStyle = WindowStyle.None;
            ResizeMode = ResizeMode.NoResize;
            AllowsTransparency = true;
            Background = Brushes.Transparent;
            SizeToContent = SizeToContent.WidthAndHeight;
            ShowInTaskbar = owner == null;
            Title = L.T("Presets");
            SetResourceReference(FontFamilyProperty, "UiFont");
            SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");

            var intro = new TextBlock
            {
                Text = L.T("A preset fills in the picture, effect and lighting settings on the Graphics page. They stay editable there."),
                TextWrapping = TextWrapping.Wrap,
                FontSize = 13,
                Margin = new Thickness(0, 8, 0, 14),
            };
            intro.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");

            var close = new Button { Content = L.T("Close"), Appearance = ControlAppearance.Primary, MinWidth = 100, IsDefault = true, IsCancel = true };
            close.Click += (_, _) => Close();
            var bar = new DockPanel { Margin = new Thickness(0, 18, 0, 0), LastChildFill = false };
            DockPanel.SetDock(close, Dock.Right);
            bar.Children.Add(close);

            var card = new Border
            {
                Margin = new Thickness(24),
                Padding = new Thickness(22, 20, 22, 18),
                CornerRadius = new CornerRadius(16),
                BorderThickness = new Thickness(1),
                Child = new StackPanel { Width = 600, Children = { LauncherPage.Text(L.T("Presets"), "PageTitle"), intro, _rows, bar } },
                Effect = new DropShadowEffect { BlurRadius = 40, ShadowDepth = 8, Direction = 270, Opacity = 0.55, Color = Colors.Black },
            };
            card.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
            card.SetResourceReference(Border.BorderBrushProperty, "DialogStrokeBrush");
            Content = card;
            Refresh();
            UiScale.Attach(this, resize: false);
        }

        /// <summary>The preset in use shows Selected; "Go back" shows while values from before the first preset are kept.</summary>
        private void Refresh()
        {
            _rows.Children.Clear();
            foreach (var preset in Presets.All)
            {
                var current = Presets.IsCurrent(preset, Ws.Ini);
                var use = LauncherPage.SmallButton(current ? L.T("Selected") : L.T("Use"),
                    current ? SymbolRegular.Checkmark24 : SymbolRegular.Play24,
                    (_, _) => Change(ini => Presets.Apply(preset, ini),
                        L.F($"Preset in use: {preset.Name}. It takes effect the next time the game starts.")),
                    current ? ControlAppearance.Primary : ControlAppearance.Secondary);
                use.IsEnabled = !current;
                _rows.Children.Add(LauncherPage.Card(SymbolRegular.Sparkle24, preset.Name, null, LauncherPage.Row(use), out var hint));
                LauncherPage.SetHint(hint, current ? L.T("In use. ") + preset.Description : preset.Description);
            }
            if (Presets.HasPrevious)
                _rows.Children.Add(LauncherPage.Card(SymbolRegular.ArrowUndo24, L.T("Back to my settings"),
                    L.T("Puts back the values you had before the first preset you used"),
                    LauncherPage.Row(LauncherPage.SmallButton(L.T("Go back"), SymbolRegular.ArrowUndo24,
                        (_, _) => Change(Presets.GoBack, L.T("Your previous picture and lighting settings are back.")))), out _));
        }

        private void Change(Action<IniFile> change, string done)
        {
            // The game writes these settings too while it runs (System menu, Insert menu).
            if (Ws.Game.Running)
            {
                Prompt.Tell(L.T("Close the game first"), L.T("The game changes these settings too while it runs."));
                return;
            }
            try
            {
                change(Ws.Ini);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                Prompt.Tell(L.T("Could not change the settings"), e.Message);
                return;
            }
            _changed = true;
            Ws.SaveSoon();
            Ws.Log(done, LogLevel.Good);
            Refresh();
        }
    }
}
