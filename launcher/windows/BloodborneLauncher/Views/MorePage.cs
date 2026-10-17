using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The less used pages behind one tab: mods, online, advanced settings and the game's log.</summary>
    public sealed class MorePage : LauncherPage
    {
        // Cheats only once the engine applies them.
        private static readonly (string Key, string Label, Type Page)[] Sections = new (string, string, Type)[]
        {
            ("mods", "Mods", typeof(ModsPage)),
            ("cheats", "Cheats", typeof(CheatsPage)),
            ("online", "Online", typeof(OnlinePage)),
            ("seamless", "Seamless", typeof(SeamlessPage)),
            ("advanced", "Advanced", typeof(SettingsPage)),
            ("output", "Log", typeof(OutputPage)),
        }.Where(s => s.Item1 != "cheats" || CheatsPage.Supported).ToArray();

        private readonly Dictionary<Type, LauncherPage> _pages = new();
        private readonly Frame _frame = new() { NavigationUIVisibility = System.Windows.Navigation.NavigationUIVisibility.Hidden, Focusable = false };
        private readonly Segmented _tabs;
        private int _current = -1;

        public MorePage() : base("More", "")
        {
            _tabs = new Segmented(Sections.Select(s => new Choice(s.Label, s.Key)).ToArray())
            {
                HorizontalAlignment = HorizontalAlignment.Center,
                Width = Sections.Length * 104,
                Margin = new Thickness(0, 6, 0, 2),
            };
            _tabs.Changed += Open;
            _frame.Navigated += (_, _) => { while (_frame.CanGoBack) _frame.RemoveBackEntry(); };

            var root = new Grid();
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            root.RowDefinitions.Add(new RowDefinition());
            root.Children.Add(_tabs);
            Grid.SetRow(_frame, 1);
            root.Children.Add(_frame);
            Content = root;
            Open(0);
        }

        public void ShowSection(string key)
        {
            var index = Array.FindIndex(Sections, s => s.Key == key);
            if (index >= 0) Open(index);
        }

        private void Open(int index)
        {
            _tabs.Select(index, notify: false);
            var type = Sections[index].Page;
            if (!_pages.TryGetValue(type, out var page))
                _pages[type] = page = (LauncherPage)Activator.CreateInstance(type)!;
            if (_current != index) _frame.Navigate(page);
            _current = index;
            page.OnShown();
        }

        public override void OnShown()
        {
            if (_current >= 0 && _pages.TryGetValue(Sections[_current].Page, out var page))
                page.OnShown();
        }
    }
}
