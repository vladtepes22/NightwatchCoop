using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// Makes the mouse wheel scroll the page. A card's ComboBox or text box would otherwise swallow
    /// the wheel even when it has nothing to scroll; this takes it in the tunnelling pass unless the
    /// control under the pointer really can scroll further that way. A list that reaches its end hands the wheel
    /// to the page only after a pause, so one swipe never scrolls both.
    /// </summary>
    public static class PageScroll
    {
        public static void Attach(ScrollViewer page)
        {
            page.PreviewMouseWheel += (_, e) =>
            {
                if (e.Handled || DropDownOpen(e.OriginalSource, page)) return;
                switch (Owner(e.OriginalSource as DependencyObject, page, e.Delta, e.Timestamp))
                {
                    case Wheel.Inner: return;
                    case Wheel.Swallow: e.Handled = true; return;
                }
                page.ScrollToVerticalOffset(page.VerticalOffset - e.Delta);
                e.Handled = true;
            };
        }

        // An open drop-down list takes the wheel: its items are in a popup, a window of their own
        // outside the page's visual tree (the walk below never reaches its ComboBox), and the
        // ComboBox holds the mouse capture while it is open.
        private static bool DropDownOpen(object source, ScrollViewer page) =>
            Mouse.Captured is ComboBox { IsDropDownOpen: true } ||
            (source is Visual visual && PresentationSource.FromVisual(visual) is { } window &&
             !ReferenceEquals(window, PresentationSource.FromVisual(page)));

        private enum Wheel { Page, Inner, Swallow }

        // The inner list that last scrolled and when (MouseEventArgs.Timestamp, ms). A touchpad keeps sending wheel steps
        // after the fingers stop: once a list reaches its end those must not carry on into the page (the lobby list, 4 Nov).
        private static ScrollViewer? _lastInner;
        private static int _lastTime;
        private const int GestureGapMs = 300;

        private static Wheel Owner(DependencyObject? source, ScrollViewer page, int delta, int time)
        {
            while (source != null && !ReferenceEquals(source, page))
            {
                if (source is ScrollViewer inner && inner.ScrollableHeight > 0)
                {
                    var room = delta > 0 ? inner.VerticalOffset > 0 : inner.VerticalOffset < inner.ScrollableHeight;
                    var sameGesture = ReferenceEquals(inner, _lastInner) && unchecked(time - _lastTime) is >= 0 and < GestureGapMs;
                    if (room || sameGesture)
                    {
                        _lastInner = inner;
                        _lastTime = time;
                        return room ? Wheel.Inner : Wheel.Swallow;
                    }
                }
                if (source is ComboBox || source is Slider)
                    return Wheel.Inner;
                // The binding capture pad takes the wheel itself.
                if (source is FrameworkElement { Tag: "capture-wheel" })
                    return Wheel.Inner;

                source = source is Visual or System.Windows.Media.Media3D.Visual3D
                    ? VisualTreeHelper.GetParent(source)
                    : LogicalTreeHelper.GetParent(source);
            }
            return Wheel.Page;
        }
    }
}
