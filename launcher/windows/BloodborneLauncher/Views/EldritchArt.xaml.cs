using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The Eldritch theme's art layer: the eyes in the arches blink now and then and turn a little
    /// towards the pointer; the grain is generated once. Hidden and stopped in every other theme.
    /// </summary>
    public partial class EldritchArt : UserControl
    {
        // (bay, offset from the bay's middle, how far down): two bays out of five, set off-centre.
        private static readonly (int Bay, double X, double Y)[] EyePlaces = { (1, 18, 30), (4, -26, 38), (4, 6, 52) };

        private readonly List<(Canvas Eye, TranslateTransform Iris)> _eyes = new();
        private readonly DispatcherTimer _blink = new() { Interval = TimeSpan.FromSeconds(1) };
        private readonly Random _random = new();

        public EldritchArt()
        {
            InitializeComponent();
            Grain.Fill = PaperGrain();
            var bays = new Grid[5];
            for (int i = 0; i < bays.Length; i++) EyeBays.Children.Add(bays[i] = new Grid());
            foreach (var (bay, x, y) in EyePlaces)
            {
                var (eye, iris) = Eye();
                eye.HorizontalAlignment = HorizontalAlignment.Center;
                eye.VerticalAlignment = VerticalAlignment.Top;
                eye.Margin = new Thickness(x * 2, y, 0, 0);
                bays[bay].Children.Add(eye);
                _eyes.Add((eye, iris));
            }
            _blink.Tick += (_, _) =>
            {
                // About one blink every eight seconds across all of them, never in step.
                if (_random.NextDouble() < 0.12) Blink(_eyes[_random.Next(_eyes.Count)].Eye);
            };
        }

        public void Show(bool on)
        {
            Visibility = on ? Visibility.Visible : Visibility.Collapsed;
            if (on) _blink.Start(); else _blink.Stop();
        }

        /// <summary>The irises lean towards the pointer (window coordinates), a couple of pixels at most.</summary>
        public void Look(Point pointer)
        {
            if (Visibility != Visibility.Visible) return;
            foreach (var (eye, iris) in _eyes)
            {
                if (!eye.IsLoaded) continue;
                var centre = eye.TranslatePoint(new Point(13, 6), this);
                var dx = pointer.X - centre.X;
                var dy = pointer.Y - centre.Y;
                var length = Math.Max(1, Math.Sqrt(dx * dx + dy * dy));
                iris.X = 3.5 * dx / length;
                iris.Y = 1.2 * dy / length;
            }
        }

        private static (Canvas, TranslateTransform) Eye()
        {
            var eye = new Canvas
            {
                Width = 26, Height = 12, Opacity = 0.4,
                RenderTransformOrigin = new Point(0.5, 0.5), RenderTransform = new ScaleTransform(1, 1),
            };
            var almond = Geometry.Parse("M 0,6 Q 13,-3 26,6 Q 13,15 0,6 Z");
            eye.Children.Add(new Path { Data = almond, Fill = Brush(0x60, 0x0A, 0x06, 0x06), Stroke = Brush(0x88, 0x9F, 0xB1, 0xBC), StrokeThickness = 0.8 });
            var iris = new TranslateTransform();
            var pupil = new Canvas { RenderTransform = iris };
            pupil.Children.Add(Placed(new Ellipse { Width = 7, Height = 7, Fill = Brush(0xB0, 0x9F, 0xB1, 0xBC) }, 9.5, 2.5));
            pupil.Children.Add(Placed(new Ellipse { Width = 2, Height = 5, Fill = Brush(0xFF, 0x0A, 0x06, 0x06) }, 12, 3.5));
            eye.Children.Add(pupil);
            eye.Clip = almond;
            return (eye, iris);
        }

        private static void Blink(Canvas eye)
        {
            var close = new DoubleAnimation(1, 0.06, TimeSpan.FromMilliseconds(110)) { AutoReverse = true, EasingFunction = new SineEase() };
            ((ScaleTransform)eye.RenderTransform).BeginAnimation(ScaleTransform.ScaleYProperty, close);
        }

        private static UIElement Placed(UIElement element, double left, double top)
        {
            Canvas.SetLeft(element, left);
            Canvas.SetTop(element, top);
            return element;
        }

        private static SolidColorBrush Brush(byte a, byte r, byte g, byte b)
        {
            var brush = new SolidColorBrush(Color.FromArgb(a, r, g, b));
            brush.Freeze();
            return brush;
        }

        /// <summary>Uneven warm specks, tiled: an old page rather than a flat fill.</summary>
        private static ImageBrush PaperGrain()
        {
            const int size = 160;
            var pixels = new byte[size * size * 4];
            var random = new Random(1888);
            for (int i = 0; i < pixels.Length; i += 4)
            {
                var speck = random.NextDouble();
                byte alpha = (byte)(speck > 0.985 ? 34 : speck * 12);
                // Premultiplied BGRA, a brown close to dried ink.
                pixels[i] = (byte)(0x14 * alpha / 255);
                pixels[i + 1] = (byte)(0x22 * alpha / 255);
                pixels[i + 2] = (byte)(0x3A * alpha / 255);
                pixels[i + 3] = alpha;
            }
            var bitmap = BitmapSource.Create(size, size, 96, 96, PixelFormats.Pbgra32, null, pixels, size * 4);
            bitmap.Freeze();
            var brush = new ImageBrush(bitmap)
            {
                TileMode = TileMode.Tile, Stretch = Stretch.None,
                Viewport = new Rect(0, 0, size, size), ViewportUnits = BrushMappingMode.Absolute,
            };
            brush.Freeze();
            return brush;
        }
    }
}
