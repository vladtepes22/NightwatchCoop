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
    /// The co-op setup wizard: an intro and one page per rule (Core/CoopPresets.cs), the scaling page
    /// holding the Vanilla / Seamless / Nightwatch presets. Every page starts on what the player has now
    /// (Keep mine when that matches no choice); nothing is written until Finish. It opens once from the
    /// Co-op tab and again from its "Run setup again" card.
    /// </summary>
    public sealed class CoopWizard : Window
    {
        private static Workspace Ws => Workspace.Current;

        private readonly CoopPresets.Page[] _pages = CoopPresets.Pages();
        /// <summary>The chosen choice per page; -1 is "Keep mine".</summary>
        private readonly int[] _picks;
        private int _step; // 0 is the intro, then one per page
        private bool _finished;

        private readonly TextBlock _title, _intro;
        private readonly StackPanel _choices;
        private readonly Button _back, _next;

        /// <summary>Opens the wizard (modal). Marks it shown, and rebuilds the pages when values were written.</summary>
        public static void Open()
        {
            var wizard = new CoopWizard();
            wizard.ShowDialog();
            Ws.Settings.Set(CoopPresets.DoneKey, true);
            Ws.SaveSoon();
            if (wizard._finished) MainWindow.Instance?.ReloadPages();
        }

        private CoopWizard()
        {
            _picks = _pages.Select(p => CoopPresets.Match(p, key => Ws.Ini.Raw(key))).ToArray();
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

            _title = LauncherPage.Text("", "PageTitle");
            _intro = Info("", 13);
            _intro.Margin = new Thickness(0, 8, 0, 14);
            _choices = new StackPanel();

            var close = new Button { Content = L.T("Close"), Appearance = ControlAppearance.Transparent, MinWidth = 90 };
            close.Click += (_, _) => Close();
            _back = new Button { Content = L.T("Back"), MinWidth = 100, Margin = new Thickness(0, 0, 8, 0) };
            _back.Click += (_, _) => Go(_step - 1);
            _next = new Button { Appearance = ControlAppearance.Primary, MinWidth = 120, IsDefault = true };
            _next.Click += (_, _) =>
            {
                if (_step < _pages.Length) Go(_step + 1);
                else Finish();
            };
            var bar = new DockPanel { Margin = new Thickness(0, 18, 0, 0), LastChildFill = false };
            DockPanel.SetDock(close, Dock.Left);
            bar.Children.Add(close);
            var right = new StackPanel { Orientation = Orientation.Horizontal, Children = { _back, _next } };
            DockPanel.SetDock(right, Dock.Right);
            bar.Children.Add(right);

            var card = new Border
            {
                Margin = new Thickness(24),
                Padding = new Thickness(22, 20, 22, 18),
                CornerRadius = new CornerRadius(16),
                BorderThickness = new Thickness(1),
                Child = new StackPanel { Width = 560, Children = { _title, _intro, _choices, bar } },
                Effect = new DropShadowEffect { BlurRadius = 40, ShadowDepth = 8, Direction = 270, Opacity = 0.55, Color = Colors.Black },
            };
            card.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
            card.SetResourceReference(Border.BorderBrushProperty, "DialogStrokeBrush");
            Content = card;
            Go(0);
        }

        private void Go(int step)
        {
            _step = Math.Clamp(step, 0, _pages.Length);
            _back.IsEnabled = _step > 0;
            _next.Content = _step < _pages.Length ? L.T("Next") : L.T("Finish");
            _next.Appearance = _step == 0 ? ControlAppearance.Secondary : ControlAppearance.Primary;
            _next.IsDefault = _step > 0;
            _choices.Children.Clear();
            if (_step == 0)
            {
                _title.Text = L.T("Co-op setup");
                _intro.Text = L.T("A few questions about how hard the party should be. Each page starts on what you have now, and nothing changes until you press Finish. Every rule stays editable on this tab.");
                var recommended = new Button { Content = L.T("Use recommended"), Appearance = ControlAppearance.Primary, MinWidth = 160, IsDefault = true, Margin = new Thickness(0, 0, 0, 6) };
                recommended.Click += (_, _) =>
                {
                    CoopPresets.ApplyRecommended((key, value) => Ws.Ini.Set(key, value));
                    _finished = true;
                    Ws.SaveSoon();
                    Close();
                };
                _choices.Children.Add(recommended);
                _choices.Children.Add(Info(L.T("Sets all of them at once: Seamless scaling, 30 % guest health cut, no rot, level sync, boss drops for guests, no travel vote. Next asks you instead."), 12));
                return;
            }
            var index = _step - 1;
            var page = _pages[index];
            _title.Text = $"{page.Title} ({_step}/{_pages.Length})";
            _intro.Text = page.Intro;
            var cards = page.Choices.Select(c => (c.Name, c.Info)).ToList();
            if (_picks[index] < 0) cards.Add((L.T("Keep mine"), L.T("Your values match none of these choices; they stay as they are.")));
            for (var i = 0; i < cards.Count; i++)
            {
                var value = i < page.Choices.Length ? i : -1;
                var radio = new RadioButton
                {
                    GroupName = "choice",
                    IsChecked = _picks[index] == value,
                    Margin = new Thickness(0, 0, 0, 8),
                    Content = new StackPanel { Children = { new TextBlock { Text = cards[i].Name, FontWeight = FontWeights.SemiBold }, Info(cards[i].Info, 12) } },
                };
                radio.Checked += (_, _) => _picks[index] = value;
                _choices.Children.Add(radio);
            }
        }

        private static TextBlock Info(string text, double size)
        {
            var block = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, FontSize = size };
            block.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            return block;
        }

        /// <summary>Writes the picked choices (Keep mine writes nothing) and closes.</summary>
        private void Finish()
        {
            for (var i = 0; i < _pages.Length; i++)
                if (_picks[i] >= 0)
                    foreach (var (key, value) in _pages[i].Choices[_picks[i]].Values)
                        Ws.Ini.Set(key, value);
            _finished = true;
            Ws.SaveSoon();
            Close();
        }
    }
}
