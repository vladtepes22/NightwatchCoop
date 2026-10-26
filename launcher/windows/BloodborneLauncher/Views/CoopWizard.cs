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
    /// The co-op setup: one page with the presets of Core/CoopPresets.cs (Vanilla, Seamless, Nightwatch),
    /// or Keep mine when the current rule values match none of them. It starts on what the player has now
    /// and writes nothing until Apply. It opens once from the Co-op tab and again from its "Run setup again" card.
    /// </summary>
    public sealed class CoopWizard : Window
    {
        private static Workspace Ws => Workspace.Current;

        private readonly CoopPresets.Preset[] _presets = CoopPresets.Presets();
        /// <summary>The chosen preset; -1 is "Keep mine".</summary>
        private int _pick;
        private bool _applied;

        /// <summary>Opens the wizard (modal). Marks it shown, and rebuilds the pages when values were written.</summary>
        public static void Open()
        {
            var wizard = new CoopWizard();
            wizard.ShowDialog();
            Ws.Settings.Set(CoopPresets.DoneKey, true);
            Ws.SaveSoon();
            if (wizard._applied) MainWindow.Instance?.ReloadPages();
        }

        private CoopWizard()
        {
            _pick = CoopPresets.Match(_presets, key => Ws.Ini.Raw(key));
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
            Title = L.T("Co-op setup");
            SetResourceReference(FontFamilyProperty, "UiFont");
            SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");

            var intro = Info(L.T("How hard should the party be? This sets the party rules on this tab (health cut, scaling, respawn, supplies, rot). Nothing changes until you press Apply, and every rule stays editable here."), 13);
            intro.Margin = new Thickness(0, 8, 0, 14);
            var choices = new StackPanel();
            var cards = _presets.Select(p => (p.Name, p.Info)).ToList();
            if (_pick < 0) cards.Add((L.T("Keep mine"), L.T("Your rules match none of these presets; they stay as they are.")));
            for (var i = 0; i < cards.Count; i++)
            {
                var value = i < _presets.Length ? i : -1;
                var radio = new RadioButton
                {
                    GroupName = "preset",
                    IsChecked = _pick == value,
                    Margin = new Thickness(0, 0, 0, 8),
                    Content = new StackPanel { Children = { new TextBlock { Text = cards[i].Name, FontWeight = FontWeights.SemiBold }, Info(cards[i].Info, 12) } },
                };
                radio.Checked += (_, _) => _pick = value;
                choices.Children.Add(radio);
            }

            var close = new Button { Content = L.T("Close"), Appearance = ControlAppearance.Transparent, MinWidth = 90 };
            close.Click += (_, _) => Close();
            var apply = new Button { Content = L.T("Apply"), Appearance = ControlAppearance.Primary, MinWidth = 120, IsDefault = true };
            apply.Click += (_, _) => Apply();
            var bar = new DockPanel { Margin = new Thickness(0, 18, 0, 0), LastChildFill = false };
            DockPanel.SetDock(close, Dock.Left);
            DockPanel.SetDock(apply, Dock.Right);
            bar.Children.Add(close);
            bar.Children.Add(apply);

            var card = new Border
            {
                Margin = new Thickness(24),
                Padding = new Thickness(22, 20, 22, 18),
                CornerRadius = new CornerRadius(16),
                BorderThickness = new Thickness(1),
                Child = new StackPanel { Width = 560, Children = { LauncherPage.Text(L.T("Co-op setup"), "PageTitle"), intro, choices, bar } },
                Effect = new DropShadowEffect { BlurRadius = 40, ShadowDepth = 8, Direction = 270, Opacity = 0.55, Color = Colors.Black },
            };
            card.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
            card.SetResourceReference(Border.BorderBrushProperty, "DialogStrokeBrush");
            Content = card;
        }

        private static TextBlock Info(string text, double size)
        {
            var block = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, FontSize = size };
            block.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            return block;
        }

        /// <summary>Writes the picked preset (Keep mine writes nothing) and closes.</summary>
        private void Apply()
        {
            if (_pick >= 0)
            {
                foreach (var (key, value) in _presets[_pick].Values)
                    Ws.Ini.Set(key, value);
                _applied = true;
                Ws.SaveSoon();
            }
            Close();
        }
    }
}
