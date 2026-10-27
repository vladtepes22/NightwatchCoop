using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace BloodborneLauncher.Views;

/// <summary>The boot splash: the blood moon GIF (moon turns red, a lamp lights) while the game starts.
/// The launcher is minimized meanwhile; the splash fades out when the game's window shows (or the game ends).</summary>
internal sealed class SplashWindow : Window
{
    // The GIF plays once (66 frames, 3.3 s) and holds on the lit lamp before the fade.
    private static readonly TimeSpan MinShown = TimeSpan.FromSeconds(3.7);
    private static readonly TimeSpan MaxShown = TimeSpan.FromSeconds(90);
    private static readonly Uri GifUri = new("pack://application:,,,/Assets/red_moon_splash.gif");

    private readonly Func<bool> _gameRunning;
    private readonly DateTime _opened = DateTime.UtcNow;
    private readonly DispatcherTimer _poll = new() { Interval = TimeSpan.FromMilliseconds(400) };
    private readonly ScaleTransform _scale = new(1, 1);
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
        SizeToContent = SizeToContent.WidthAndHeight;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        var gif = new Image { Width = 316, Height = 316, Stretch = Stretch.Uniform };
        RenderOptions.SetBitmapScalingMode(gif, BitmapScalingMode.HighQuality);
        Content = Build(gif);
        Opacity = 0;
        Loaded += (_, _) =>
        {
            // Start the GIF with the fade-in so its first frame is the one people see.
            Play(gif);
            BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(500)));
        };
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

    private UIElement Build(Image gif)
    {
        var shape = new CornerRadius(14);
        // The glow sits on its own layer behind the card: an effect on the card itself would re-render
        // the animated GIF through it every frame. The window margin keeps the glow from being clipped.
        var glow = new Border
        {
            CornerRadius = shape,
            Background = new SolidColorBrush(Color.FromRgb(0x0E, 0x0E, 0x14)),
            Effect = new DropShadowEffect { BlurRadius = 60, ShadowDepth = 0, Color = Color.FromRgb(0xC8, 0x23, 0x1B), Opacity = 0.45 },
        };
        var card = new Border
        {
            Width = 360,
            Padding = new Thickness(22, 22, 22, 18),
            CornerRadius = shape,
            Background = new SolidColorBrush(Color.FromRgb(0x0E, 0x0E, 0x14)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x2A, 0x14, 0x16)),
            BorderThickness = new Thickness(1),
            Child = new StackPanel
            {
                Children =
                {
                    gif,
                    new TextBlock
                    {
                        Text = "Nightwatch", FontFamily = new FontFamily("Palatino Linotype, Georgia"), FontSize = 26, FontWeight = FontWeights.Bold,
                        Foreground = new SolidColorBrush(Color.FromRgb(0xED, 0xE3, 0xCF)), HorizontalAlignment = HorizontalAlignment.Center,
                        Margin = new Thickness(0, 14, 0, 2),
                        Effect = new DropShadowEffect { BlurRadius = 14, ShadowDepth = 0, Color = Color.FromRgb(0xC8, 0x23, 0x1B), Opacity = 0.7 },
                    },
                    new TextBlock
                    {
                        Text = "C O - O P", FontFamily = new FontFamily("Palatino Linotype, Georgia"), FontSize = 12,
                        Foreground = new SolidColorBrush(Color.FromRgb(0x9A, 0x8F, 0x7C)), HorizontalAlignment = HorizontalAlignment.Center,
                    },
                },
            },
        };
        return new Grid
        {
            Margin = new Thickness(48),
            RenderTransformOrigin = new Point(0.5, 0.5),
            RenderTransform = _scale,
            Children = { glow, card },
        };
    }

    /// <summary>Plays the GIF once and holds its last frame. Every frame of this GIF redraws the
    /// same box (disposal "restore background"), so each one is drawn on its own at its offset.</summary>
    private static void Play(Image target)
    {
        BitmapDecoder decoder;
        try { decoder = BitmapDecoder.Create(GifUri, BitmapCreateOptions.None, BitmapCacheOption.OnLoad); }
        catch { return; } // a missing asset only costs the picture, never the start
        var size = new Rect(0, 0, decoder.Frames[0].PixelWidth, decoder.Frames[0].PixelHeight);
        if (decoder.Metadata is BitmapMetadata gifMeta)
        {
            if (gifMeta.GetQuery("/logscrdesc/Width") is ushort w) size.Width = w;
            if (gifMeta.GetQuery("/logscrdesc/Height") is ushort h) size.Height = h;
        }
        var frames = new ObjectAnimationUsingKeyFrames { FillBehavior = FillBehavior.HoldEnd };
        var at = TimeSpan.Zero;
        foreach (var frame in decoder.Frames)
        {
            var meta = frame.Metadata as BitmapMetadata;
            double left = meta?.GetQuery("/imgdesc/Left") is ushort l ? l : 0;
            double top = meta?.GetQuery("/imgdesc/Top") is ushort t ? t : 0;
            var delay = meta?.GetQuery("/grctlext/Delay") is ushort d && d > 0 ? d * 10 : 50;
            // An empty rectangle the size of the whole GIF keeps every frame at its place in the canvas.
            var drawing = new DrawingGroup();
            drawing.Children.Add(new GeometryDrawing(Brushes.Transparent, null, new RectangleGeometry(size)));
            drawing.Children.Add(new ImageDrawing(frame, new Rect(left, top, frame.PixelWidth, frame.PixelHeight)));
            drawing.Freeze();
            var image = new DrawingImage(drawing);
            image.Freeze();
            frames.KeyFrames.Add(new DiscreteObjectKeyFrame(image, KeyTime.FromTimeSpan(at)));
            at += TimeSpan.FromMilliseconds(delay);
        }
        if (frames.KeyFrames.Count == 0) return;
        target.Source = (ImageSource)frames.KeyFrames[0].Value;
        target.BeginAnimation(Image.SourceProperty, frames);
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
        var time = TimeSpan.FromMilliseconds(600);
        var fade = new DoubleAnimation(0, time);
        fade.Completed += (_, _) =>
        {
            Close();
            Finished?.Invoke(gameEnded);
        };
        _scale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(0.96, time));
        _scale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(0.96, time));
        BeginAnimation(OpacityProperty, fade);
    }
}
