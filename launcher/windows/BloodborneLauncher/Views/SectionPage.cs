using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>A tab whose pages are sections picked by a row of buttons across its top (More, Workshop).</summary>
    public abstract class SectionPage : LauncherPage
    {
        private readonly Dictionary<Type, LauncherPage> _pages = new();
        private readonly Frame _frame = new() { NavigationUIVisibility = System.Windows.Navigation.NavigationUIVisibility.Hidden, Focusable = false };
        private readonly Segmented _tabs;
        private int _current = -1;
        private readonly (string Key, string Label, Type? Page)[] Sections;

        /// <summary>A section with no page (null) is a button: picking it calls this and the tab keeps showing the page it was on.</summary>
        protected virtual void Activate(string key) { }

        protected SectionPage(string title, (string Key, string Label, Type? Page)[] sections) : base(title, "")
        {
            Sections = sections;
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

        public IEnumerable<string> SectionKeys => Sections.Select(s => s.Key);

        /// <summary>Makes a section's page without showing it (the settings search, in the background).</summary>
        public void MakeSection(string key)
        {
            var index = Array.FindIndex(Sections, s => s.Key == key);
            if (index >= 0 && Sections[index].Page is { } page) PageOf(page);
        }

        /// <summary>The sections whose page is made: the settings search looks through them as pages
        /// of their own (MakeSection makes the others).</summary>
        public IEnumerable<(string Key, string Label, LauncherPage Page)> MadeSectionPages() =>
            Sections.Where(s => s.Page != null && _pages.ContainsKey(s.Page)).Select(s => (s.Key, s.Label, _pages[s.Page!]));

        private LauncherPage PageOf(Type type)
        {
            if (!_pages.TryGetValue(type, out var page))
                _pages[type] = page = (LauncherPage)Activator.CreateInstance(type)!;
            return page;
        }

        private void Open(int index)
        {
            if (Sections[index].Page is not { } type)
            {
                if (_current >= 0) _tabs.Select(_current, notify: false);
                Activate(Sections[index].Key);
                return;
            }
            _tabs.Select(index, notify: false);
            var page = PageOf(type);
            if (_current != index) _frame.Navigate(page);
            _current = index;
            page.OnShown();
        }

        public override void OnShown()
        {
            if (_current >= 0 && Sections[_current].Page is { } type && _pages.TryGetValue(type, out var page))
                page.OnShown();
        }
    }
}
