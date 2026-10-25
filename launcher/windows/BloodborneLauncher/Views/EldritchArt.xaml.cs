using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The Eldritch theme's living backdrop. Painted layers (Themes/Eldritch, built into the exe) placed by
    /// window size: the city, the moon, two fog bands drifting apart, and every half minute to a minute and
    /// a half one surprise on the Stage in front of the panes: a queen floating in prayer, a crow across the
    /// moon, the moon opening an eye, a hand reaching up, or a grinning head on spider legs peeking up.
    /// Only transforms, opacity and frame swaps move, at 30 frames a second, and all of it sleeps while
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
        private readonly DispatcherTimer _tick = new() { Interval = TimeSpan.FromSeconds(1) };
        private readonly Random _random = new();
        private Image? _moon, _moonEye;
        private ImageBrush? _fogBack, _fogFront;
        private bool _built, _shown, _awake = true;
        private DateTime _nextSurprise;
        // Each surprise once per round, in a shuffled order; the queen opens the first round.
        private readonly List<int> _bag = new();
        private bool _firstRound = true;

        public EldritchArt()
        {
            InitializeComponent();
            SizeChanged += (_, _) => Layout();
            _tick.Tick += (_, _) => Tick();
        }

        /// <summary>Where the surprises appear: a click-through canvas over the whole window, in front of
        /// the panes (the panes would hide them). Without one they stay behind, on this control.</summary>
        public Panel? Stage { get; set; }

        private Panel Front => Stage ?? Surprises;

        public void Show(bool on)
        {
            _shown = on;
            Visibility = on ? Visibility.Visible : Visibility.Collapsed;
            if (!on) Front.Children.Clear();
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
            if (on) _tick.Start(); else _tick.Stop();
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
            {
                _moon = Add(Sky, moon);
                if (Picture("moon-eye.png", 1200) is { } eye)
                {
                    _moonEye = Add(Sky, eye);
                    _moonEye.Opacity = 0;
                }
            }

            if (Picture("fog.png", 2048) is { } fog)
            {
                _fogBack = FogBrush(fog, 1.0);
                _fogFront = FogBrush(fog, 0.75);
                FogBack.Fill = _fogBack;
                FogFront.Fill = _fogFront;
                Drift(_fogBack, 2048, 140);
                Drift(_fogFront, -2048 * 0.75, 95);
            }

            _nextSurprise = DateTime.Now.AddSeconds(19 + _random.Next(12));
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
            foreach (var image in new[] { _moon, _moonEye })
            {
                if (image == null) continue;
                image.Width = image.Height = size;
                Canvas.SetLeft(image, w - 0.16 * w - size / 2);
                Canvas.SetTop(image, 0.16 * h - size / 2 + 0.08 * size);
            }
        }

        // ---------------------------------------------------------------- life

        private void Tick()
        {
            var now = DateTime.Now;
            if (now < _nextSurprise) return;
            _nextSurprise = now.AddSeconds(35 + _random.Next(58));
            Surprise();
        }

        /// <summary>Swaps an image through frames, <paramref name="stepMs"/> apart, ending on the last.</summary>
        public static void Frames(Image image, IReadOnlyList<ImageSource> frames, int stepMs, int delayMs)
        {
            var animation = new ObjectAnimationUsingKeyFrames { BeginTime = TimeSpan.FromMilliseconds(delayMs) };
            for (int i = 0; i < frames.Count; i++)
                animation.KeyFrames.Add(new DiscreteObjectKeyFrame(frames[i], TimeSpan.FromMilliseconds(i * stepMs)));
            animation.Completed += (_, _) =>
            {
                image.BeginAnimation(Image.SourceProperty, null);
                image.Source = frames[^1];
            };
            image.BeginAnimation(Image.SourceProperty, animation);
        }

        private void Surprise()
        {
            var options = new List<(int Id, Action Run)>();
            if (Picture("queen.png", 768) is { } queen) options.Add((0, () => Queen(queen)));
            if (Picture("crow-1.png", 256) is { } up && Picture("crow-2.png", 256) is { } down) options.Add((1, () => Crow(up, down)));
            if (_moonEye != null) options.Add((2, MoonEye));
            if (Picture("hand.png", 512) is { } hand) options.Add((3, () => Rise(hand, 0.30, 1200, 1400, _random.Next(2) == 0)));
            if (Picture("spider.png", 768) is { } spider) options.Add((4, () => Rise(spider, 0.26, 900, 2800, right: true)));
            if (options.Count == 0) return;
            if (_bag.Count == 0)
            {
                _bag.AddRange(options.Select(o => o.Id).OrderBy(_ => _random.Next()));
                if (_firstRound && _bag.Remove(0)) _bag.Insert(0, 0);
                _firstRound = false;
            }
            var id = _bag[0];
            _bag.RemoveAt(0);
            options.FirstOrDefault(o => o.Id == id).Run?.Invoke();
        }

        private Image Sprite(BitmapSource source, double height)
        {
            var image = Add(Front, source);
            image.IsHitTestVisible = false;
            image.Height = height;
            image.Width = height * source.PixelWidth / source.PixelHeight;
            return image;
        }

        private void Remove(Image image, Timeline last)
        {
            var stage = Front;
            last.Completed += (_, _) => stage.Children.Remove(image);
        }

        /// <summary>A queen in prayer floats across from the right edge to the left, bobbing slowly, fading
        /// in as she enters and out as she leaves.</summary>
        private void Queen(BitmapSource picture)
        {
            double w = ActualWidth, h = ActualHeight;
            var image = Sprite(picture, 0.42 * h);
            Canvas.SetTop(image, 0.22 * h);
            var span = TimeSpan.FromSeconds(16);
            var glide = new DoubleAnimation(w - 0.4 * image.Width, -0.6 * image.Width, span);
            var seen = new DoubleAnimationUsingKeyFrames();
            seen.KeyFrames.Add(new EasingDoubleKeyFrame(0, TimeSpan.Zero));
            seen.KeyFrames.Add(new EasingDoubleKeyFrame(0.85, TimeSpan.FromSeconds(2.5), new SineEase()));
            seen.KeyFrames.Add(new EasingDoubleKeyFrame(0.85, TimeSpan.FromSeconds(13.5)));
            seen.KeyFrames.Add(new EasingDoubleKeyFrame(0, span, new SineEase()));
            var hover = new DoubleAnimation(0, -0.03 * h, TimeSpan.FromSeconds(3)) { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever,
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut } };
            var move = new TranslateTransform();
            image.RenderTransform = move;
            image.Opacity = 0;
            Timeline.SetDesiredFrameRate(seen, 30);
            Timeline.SetDesiredFrameRate(hover, 30);
            Timeline.SetDesiredFrameRate(glide, 30);
            Canvas.SetLeft(image, w);
            Remove(image, seen);
            image.BeginAnimation(Canvas.LeftProperty, glide);
            image.BeginAnimation(OpacityProperty, seen);
            move.BeginAnimation(TranslateTransform.YProperty, hover);
        }

        /// <summary>A crow crosses in front of the moon, flapping.</summary>
        private void Crow(BitmapSource up, BitmapSource down)
        {
            double w = ActualWidth, h = ActualHeight;
            var image = Sprite(up, 0.09 * h);
            Canvas.SetTop(image, 0.06 * h + _random.NextDouble() * 0.12 * h);
            var fly = new DoubleAnimation(w + 10, w * 0.45, TimeSpan.FromSeconds(5.5));
            var drop = new DoubleAnimation(0, 0.05 * h, TimeSpan.FromSeconds(5.5));
            var flap = new ObjectAnimationUsingKeyFrames { RepeatBehavior = RepeatBehavior.Forever, Duration = TimeSpan.FromMilliseconds(260) };
            flap.KeyFrames.Add(new DiscreteObjectKeyFrame(up, TimeSpan.Zero));
            flap.KeyFrames.Add(new DiscreteObjectKeyFrame(down, TimeSpan.FromMilliseconds(130)));
            var fade = new DoubleAnimation(1, 0, TimeSpan.FromSeconds(1.2)) { BeginTime = TimeSpan.FromSeconds(4.3) };
            var move = new TranslateTransform();
            image.RenderTransform = move;
            Remove(image, fly);
            image.BeginAnimation(Canvas.LeftProperty, fly);
            move.BeginAnimation(TranslateTransform.YProperty, drop);
            image.BeginAnimation(Image.SourceProperty, flap);
            image.BeginAnimation(OpacityProperty, fade);
        }

        /// <summary>The moon opens an eye for a few seconds.</summary>
        private void MoonEye()
        {
            var look = new DoubleAnimationUsingKeyFrames();
            look.KeyFrames.Add(new EasingDoubleKeyFrame(0, TimeSpan.Zero));
            look.KeyFrames.Add(new EasingDoubleKeyFrame(1, TimeSpan.FromSeconds(1.6), new SineEase()));
            look.KeyFrames.Add(new EasingDoubleKeyFrame(1, TimeSpan.FromSeconds(4.2)));
            look.KeyFrames.Add(new EasingDoubleKeyFrame(0, TimeSpan.FromSeconds(5.0), new SineEase()));
            _moonEye!.BeginAnimation(OpacityProperty, look);
        }

        /// <summary>Something rises from the bottom edge near one side, stays, and sinks back.</summary>
        private void Rise(BitmapSource picture, double heightFraction, int riseMs, int stayMs, bool right)
        {
            double w = ActualWidth, h = ActualHeight;
            var image = Sprite(picture, heightFraction * h);
            Canvas.SetLeft(image, right ? w - image.Width - 0.04 * w : 0.04 * w);
            var top = h - image.Height;
            var rise = new DoubleAnimationUsingKeyFrames();
            rise.KeyFrames.Add(new EasingDoubleKeyFrame(h, TimeSpan.Zero));
            rise.KeyFrames.Add(new EasingDoubleKeyFrame(top, TimeSpan.FromMilliseconds(riseMs), new SineEase { EasingMode = EasingMode.EaseOut }));
            rise.KeyFrames.Add(new EasingDoubleKeyFrame(top, TimeSpan.FromMilliseconds(riseMs + stayMs)));
            rise.KeyFrames.Add(new EasingDoubleKeyFrame(h, TimeSpan.FromMilliseconds(riseMs * 2 + stayMs), new SineEase { EasingMode = EasingMode.EaseIn }));
            Timeline.SetDesiredFrameRate(rise, 30);
            Canvas.SetTop(image, h);
            Remove(image, rise);
            image.BeginAnimation(Canvas.TopProperty, rise);
        }
    }
}
