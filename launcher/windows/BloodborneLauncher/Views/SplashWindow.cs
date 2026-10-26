using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;

namespace BloodborneLauncher.Views;

/// <summary>The boot splash: a blood moon over a lit lamp while the game starts. The launcher is
/// minimized meanwhile; the splash fades out when the game's window shows (or the game ends).</summary>
internal sealed class SplashWindow : Window
{
    private static readonly TimeSpan MinShown = TimeSpan.FromSeconds(3.7);
    private static readonly TimeSpan MaxShown = TimeSpan.FromSeconds(90);

    private readonly Func<bool> _gameRunning;
    private readonly DateTime _opened = DateTime.UtcNow;
    private readonly DispatcherTimer _poll = new() { Interval = TimeSpan.FromMilliseconds(400) };
    private bool _closing;

    /// <summary>Raised once when the splash is gone; the argument is true when the game ended first.</summary>
    public event Action<bool>? Finished;

    private SplashWindow(Func<bool> gameRunning)
    {
        _gameRunning = gameRunning;
        WindowStyle = WindowStyle.None;
        AllowsTransparency = true;
        Background = Brushes.Transparent;
        ResizeMode = ResizeMode.NoResize;
        ShowInTaskbar = false;
        Topmost = true;
        ShowActivated = false;
        Width = 360;
        Height = 440;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Content = Build();
        Opacity = 0;
        Loaded += (_, _) => BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(500)));
        _poll.Tick += (_, _) => Check();
        _poll.Start();
    }

    /// <summary>Shows the splash; null when the setting turns it off ("splash_off").</summary>
    public static SplashWindow? Open(Func<bool> gameRunning, bool off)
    {
        if (off) return null;
        var splash = new SplashWindow(gameRunning);
        splash.Show();
        return splash;
    }

    private static UIElement Build()
    {
        var gold = new SolidColorBrush(Color.FromRgb(0xED, 0xE3, 0xCF));
        var moon = new MoonView(190);
        var card = new Border
        {
            Margin = new Thickness(24),
            CornerRadius = new CornerRadius(14),
            Background = new SolidColorBrush(Color.FromRgb(0x0E, 0x0E, 0x14)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x2A, 0x14, 0x16)),
            BorderThickness = new Thickness(1),
            Effect = new System.Windows.Media.Effects.DropShadowEffect { BlurRadius = 50, ShadowDepth = 0, Color = Color.FromRgb(0xC8, 0x23, 0x1B), Opacity = 0.35 },
            Child = new StackPanel
            {
                VerticalAlignment = VerticalAlignment.Center,
                Children =
                {
                    moon,
                    new TextBlock
                    {
                        Text = "Nightwatch", FontFamily = new FontFamily("Palatino Linotype, Georgia"), FontSize = 28, FontWeight = FontWeights.Bold,
                        Foreground = gold, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 22, 0, 2),
                        Effect = new System.Windows.Media.Effects.DropShadowEffect { BlurRadius = 14, ShadowDepth = 0, Color = Color.FromRgb(0xC8, 0x23, 0x1B) },
                    },
                    new TextBlock
                    {
                        Text = "C O - O P", FontFamily = new FontFamily("Palatino Linotype, Georgia"), FontSize = 12,
                        Foreground = new SolidColorBrush(Color.FromRgb(0x9A, 0x8F, 0x7C)), HorizontalAlignment = HorizontalAlignment.Center,
                    },
                },
            },
        };
        return card;
    }

    /// <summary>The moon: a red disc that rises into place, then breathes, with a small lit lamp below.</summary>
    private sealed class MoonView : Grid
    {
        public MoonView(double size)
        {
            Width = Height = size;
            ClipToBounds = false;
            var halo = new System.Windows.Shapes.Ellipse
            {
                Width = size * 1.5, Height = size * 1.5,
                Fill = new RadialGradientBrush(Color.FromArgb(0x70, 0xC8, 0x23, 0x1B), Colors.Transparent),
            };
            var disc = new System.Windows.Shapes.Ellipse
            {
                Width = size * 0.78, Height = size * 0.78,
                Fill = new RadialGradientBrush { GradientOrigin = new Point(0.4, 0.35), GradientStops =
                {
                    new GradientStop(Color.FromRgb(0xE8, 0x5A, 0x3C), 0), new GradientStop(Color.FromRgb(0xB4, 0x1A, 0x16), 0.6), new GradientStop(Color.FromRgb(0x5A, 0x0A, 0x0C), 1),
                } },
                RenderTransform = new TranslateTransform(0, 40),
            };
            var lamp = new System.Windows.Shapes.Ellipse
            {
                Width = 9, Height = 9, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 0, -8),
                Fill = new SolidColorBrush(Color.FromRgb(0xFF, 0xC8, 0x6B)), Opacity = 0,
                Effect = new System.Windows.Media.Effects.DropShadowEffect { BlurRadius = 18, ShadowDepth = 0, Color = Color.FromRgb(0xFF, 0xA0, 0x40) },
            };
            Children.Add(halo);
            Children.Add(disc);
            Children.Add(lamp);

            var rise = new DoubleAnimation(40, 0, TimeSpan.FromSeconds(2.2)) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } };
            ((TranslateTransform)disc.RenderTransform).BeginAnimation(TranslateTransform.YProperty, rise);
            lamp.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromSeconds(0.8)) { BeginTime = TimeSpan.FromSeconds(2.4) });
            halo.BeginAnimation(OpacityProperty, new DoubleAnimation(0.55, 1, TimeSpan.FromSeconds(1.8)) { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });
        }
    }

    private void Check()
    {
        if (_closing) return;
        var elapsed = DateTime.UtcNow - _opened;
        var ended = !_gameRunning();
        if (!ended && (elapsed < MinShown || (elapsed < MaxShown && !GameWindowShown()))) return;
        Fade(ended);
    }

    /// <summary>The game's own window exists once bb-probe has a visible top-level window.</summary>
    private static bool GameWindowShown()
    {
        foreach (var p in Process.GetProcessesByName("bb-probe"))
        {
            using (p)
            {
                try { if (p.MainWindowHandle != IntPtr.Zero) return true; } catch { /* exited */ }
            }
        }
        return false;
    }

    /// <summary>Fades out and closes now (also used when the start is cancelled).</summary>
    public void Fade(bool gameEnded)
    {
        if (_closing) return;
        _closing = true;
        _poll.Stop();
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(600));
        fade.Completed += (_, _) =>
        {
            Close();
            Finished?.Invoke(gameEnded);
        };
        BeginAnimation(OpacityProperty, fade);
    }
}
