using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The Eldritch theme's living backdrop. Painted layers (Themes/Eldritch, built into the exe) placed by
    /// window size: the city, the moon and two fog bands drifting apart.
    /// Only the fog moves, at 30 frames a second, and all of it sleeps while
    /// the window is inactive or minimized (Awake), so the game keeps the GPU. A missing picture leaves
    /// its element out.
    /// </summary>
    public partial class EldritchArt : UserControl
    {
        private const string Folder = "pack://application:,,,/Nightwatch Co-op;component/Themes/Eldritch/";
        private static readonly Dictionary<string, BitmapSource?> Cache = new();

        /// <summary>A picture of the theme, decoded once at no more than <paramref name="width"/> pixels;
        /// null when it is not in this build.</summary>
        public static BitmapSource? Picture(string name, int width = 0)
        {
            var key = name + "@" + width;
            if (Cache.TryGetValue(key, out var cached)) return cached;
            BitmapSource? image = null;
            try
            {
                var bitmap = new BitmapImage();
                bitmap.BeginInit();
                bitmap.UriSource = new Uri(Folder + name, UriKind.Absolute);
                bitmap.CacheOption = BitmapCacheOption.OnLoad;
                if (width > 0) bitmap.DecodePixelWidth = width;
                bitmap.EndInit();
                bitmap.Freeze();
                image = bitmap;
            }
            catch (Exception e) when (e is IOException or NotSupportedException or InvalidOperationException) { }
            return Cache[key] = image;
        }

        private readonly List<Storyboard> _ambient = new();
        private Image? _moon;
        private ImageBrush? _fogBack, _fogFront;
        private bool _built, _shown, _awake = true;

        public EldritchArt()
        {
            InitializeComponent();
            SizeChanged += (_, _) => Layout();
        }

        public void Show(bool on)
        {
            _shown = on;
            Visibility = on ? Visibility.Visible : Visibility.Collapsed;
            if (on && !_built) Build();
            Run();
        }

        /// <summary>The launcher is in front (active, not minimized): only then does anything move.</summary>
        public void Awake(bool awake)
        {
            _awake = awake;
            Run();
        }

        private void Run()
        {
            var on = _shown && _awake && _built;
            foreach (var story in _ambient)
                if (on) story.Resume(this); else story.Pause(this);
        }

        /// <summary>Launcher › Background blur on the painted city (0 = sharp). The picture never changes, so
        /// the blur is worked out once, not per frame.</summary>
        public void Blur(double radius)
        {
            Backdrop.Effect = radius <= 0 ? null : new System.Windows.Media.Effects.BlurEffect
            {
                Radius = radius, KernelType = System.Windows.Media.Effects.KernelType.Gaussian,
                RenderingBias = System.Windows.Media.Effects.RenderingBias.Performance,
            };
            // Bigger than the window, so the blur's soft edge stays outside it.
            Backdrop.Margin = new Thickness(radius <= 0 ? 0 : -radius * 1.5);
            Backdrop.CacheMode = radius <= 0 ? null : new BitmapCache();
        }

        // ---------------------------------------------------------------- building

        private void Build()
        {
            _built = true;
            Backdrop.Source = Picture("backdrop.jpg", 2560);

            if (Picture("moon.png", 1200) is { } moon)
                _moon = Add(Sky, moon);

            if (Picture("fog.png", 2048) is { } fog)
            {
                _fogBack = FogBrush(fog, 1.0);
                _fogFront = FogBrush(fog, 0.75);
                FogBack.Fill = _fogBack;
                FogFront.Fill = _fogFront;
                Drift(_fogBack, 2048, 140);
                Drift(_fogFront, -2048 * 0.75, 95);
            }

            Layout();
        }

        private static Image Add(Panel layer, BitmapSource source)
        {
            var image = new Image { Source = source, Stretch = Stretch.Uniform };
            RenderOptions.SetBitmapScalingMode(image, BitmapScalingMode.HighQuality);
            layer.Children.Add(image);
            return image;
        }

        private static ImageBrush FogBrush(BitmapSource fog, double scale) => new(fog)
        {
            TileMode = TileMode.Tile, Stretch = Stretch.Fill, ViewportUnits = BrushMappingMode.Absolute,
            Viewport = new Rect(0, 0, 2048 * scale, 512 * scale), AlignmentY = AlignmentY.Bottom,
            Transform = new TranslateTransform(),
        };

        private void Drift(ImageBrush brush, double distance, double seconds) =>
            Ambient(new DoubleAnimation(0, distance, TimeSpan.FromSeconds(seconds)) { RepeatBehavior = RepeatBehavior.Forever },
                (TranslateTransform)brush.Transform, TranslateTransform.XProperty);

        private void Ambient(DoubleAnimation animation, DependencyObject target, DependencyProperty property)
        {
            var story = new Storyboard();
            Timeline.SetDesiredFrameRate(story, 30);
            Storyboard.SetTarget(animation, target);
            Storyboard.SetTargetProperty(animation, new PropertyPath(property));
            story.Children.Add(animation);
            story.Begin(this, true);
            _ambient.Add(story);
        }

        // ---------------------------------------------------------------- placing

        private void Layout()
        {
            double w = ActualWidth, h = ActualHeight;
            if (!_built || w < 10 || h < 10 || _moon == null) return;
            var size = Math.Min(0.32 * w, 0.75 * h);
            _moon.Width = _moon.Height = size;
            Canvas.SetLeft(_moon, w - 0.16 * w - size / 2);
            Canvas.SetTop(_moon, 0.16 * h - size / 2 + 0.08 * size);
        }
    }
}
