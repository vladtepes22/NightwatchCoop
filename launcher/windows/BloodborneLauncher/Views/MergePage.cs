using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using RichTextBox = System.Windows.Controls.RichTextBox;

namespace BloodborneLauncher.Views
{
    /// <summary>Workshop > Merge: picks mods and an order, and writes them into one new mod folder (the merger tool).
    /// It does not enable the new mod: the player does that on the Mods tab.</summary>
    public sealed class MergePage : LauncherPage
    {
        /// <summary>The most log lines the box keeps; the log file has them all.</summary>
        private const int ShownLines = 3000;

        private readonly StackPanel _list = new();
        private readonly TextBlock _empty;
        private readonly Wpf.Ui.Controls.TextBox _nameBox;
        private readonly Button _merge;
        private readonly TextBlock _status;
        private readonly RichTextBox _log;
        private readonly Paragraph _lines = new() { Margin = new Thickness(0) };
        private readonly List<(string Name, bool On)> _items = new();
        private int _shown;
        private bool _busy;

        public MergePage() : base(L.T("Merger"), L.T("Mod order decides the winner when two mods change the same thing."))
        {
            Group(L.T("Mods to merge"));
            _empty = Text(L.T("No mods are installed."), "Faint");
            Body.Children.Add(_empty);
            Body.Children.Add(_list);

            Group(L.T("New mod"));
            _nameBox = new Wpf.Ui.Controls.TextBox
            {
                PlaceholderText = L.T("Name of the new mod"), PlaceholderEnabled = true, Width = 240, MaxLength = Merger.MaxName,
                VerticalAlignment = VerticalAlignment.Center,
            };
            _nameBox.TextChanged += (_, _) => Sync();
            _merge = SmallButton(L.T("Merge"), SymbolRegular.Merge24, async (_, _) => await MergeNow(), ControlAppearance.Primary);
            Add(Card(SymbolRegular.Box24, L.T("Merge into one mod"),
                L.T("Writes a new folder in the Mods folder. Turn it on in the Mods tab afterwards."), Row(_nameBox, _merge), out _status));

            Group(L.T("Last merge"));
            _log = new RichTextBox
            {
                IsReadOnly = true, IsDocumentEnabled = false, Height = 280, Padding = new Thickness(8),
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
                BorderThickness = new Thickness(1), FontSize = 12, Document = new FlowDocument(_lines) { PageWidth = 4000 },
            };
            _log.Document.SetResourceReference(FlowDocument.FontFamilyProperty, "MonoFont"); // a FlowDocument has its own default font
            _log.Document.FontSize = 12;
            _log.SetResourceReference(Control.ForegroundProperty, "TextFillColorPrimaryBrush");
            _log.SetResourceReference(Control.BackgroundProperty, "PanelDeepBrush");
            _log.SetResourceReference(Control.BorderBrushProperty, "CardStrokeColorDefaultBrush");
            var open = SmallButton(L.T("Open full log"), SymbolRegular.Open24, (_, _) => OpenLog(), ControlAppearance.Transparent);
            open.HorizontalAlignment = HorizontalAlignment.Left;
            open.Margin = new Thickness(0, 0, 0, 6);
            Body.Children.Add(open);
            Body.Children.Add(_log);
            ShowPrevious();
            Sync();
        }

        public override void OnShown()
        {
            if (_busy) return;
            var installed = Mods.Load(GameEnvironment.ModsDir(S)).Where(m => !Randomizer.IsWorld(m.Name)).Select(m => m.Name).ToList();
            // What the player ordered stays; new folders join at the end in the Mods page order, gone ones leave.
            var kept = _items.Where(i => installed.Contains(i.Name)).ToList();
            _items.Clear();
            _items.AddRange(kept);
            _items.AddRange(installed.Where(n => kept.All(i => i.Name != n)).Select(n => (n, false)));
            Draw();
            Sync();
        }

        private void Draw()
        {
            _list.Children.Clear();
            _empty.Visibility = _items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            for (var i = 0; i < _items.Count; i++)
            {
                var at = i;
                var (name, on) = _items[i];
                var toggle = new ToggleSwitch { IsChecked = on, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 0, 0), ToolTip = L.T("Include in the merge") };
                toggle.Click += (_, _) =>
                {
                    _items[at] = (name, toggle.IsChecked == true);
                    Sync();
                };
                var up = IconButton(SymbolRegular.ArrowUp24, L.T("Merge earlier"), (_, _) => Move(at, at - 1));
                var down = IconButton(SymbolRegular.ArrowDown24, L.T("Merge later"), (_, _) => Move(at, at + 1));
                up.IsEnabled = at > 0;
                down.IsEnabled = at < _items.Count - 1;
                var card = Card(SymbolRegular.Box24, name, L.F($"Merge order {at + 1}"), Row(up, down, toggle), out _);
                card.Margin = new Thickness(0, 0, 0, 4);
                _list.Children.Add(card);
            }
        }

        private void Move(int from, int to)
        {
            if (_busy || to < 0 || to >= _items.Count) return;
            (_items[from], _items[to]) = (_items[to], _items[from]);
            Draw();
        }

        /// <summary>The Merge button and the line under the name: 2+ mods, and a name that can be a new folder.</summary>
        private void Sync()
        {
            var ticked = _items.Count(i => i.On);
            var valid = Merger.ValidName(_nameBox.Text, GameEnvironment.ModsDir(S), out var error);
            _merge.IsEnabled = !_busy && ticked >= 2 && valid;
            _nameBox.IsEnabled = !_busy;
            _list.IsEnabled = !_busy;
            if (_busy) return;
            SetHint(_status, error.Length > 0 ? error : ticked < 2 ? L.T("Tick at least two mods to merge.") : valid ? "" : L.T("Type a name for the new mod."));
        }

        private async Task MergeNow()
        {
            var name = _nameBox.Text.Trim();
            var mods = _items.Where(i => i.On).Select(i => i.Name).ToList();
            if (_busy || mods.Count < 2 || !Merger.ValidName(name, GameEnvironment.ModsDir(S), out _)) return;
            _busy = true;
            Sync();
            SetHint(_status, L.T("Merging…"));
            Clear();
            string? error;
            try
            {
                error = await Merger.Merge(S, name, mods, line => AddLine(line));
            }
            catch (Exception e)
            {
                error = e.Message; // never takes the launcher down
            }
            _busy = false;
            var done = L.F($"Merged into {name}. It is in the Mods folder: turn it on in the Mods tab.");
            if (error == null)
            {
                AddLine("");
                AddLine(done);
                _nameBox.Text = "";
            }
            else
            {
                AddLine(error.StartsWith("ERROR", StringComparison.Ordinal) ? error : "ERROR " + error);
            }
            OnShown(); // the new mod shows in the list (the Mods tab reads the folder when it is shown)
            Sync();
            SetHint(_status, error == null ? done : L.T("The merge failed."));
        }

        private void Clear()
        {
            _lines.Inlines.Clear();
            _shown = 0;
        }

        /// <summary>One log line, tinted by its prefix. Past ShownLines the box says so once; the file has the rest.</summary>
        private void AddLine(string text)
        {
            if (_shown == ShownLines) _lines.Inlines.Add(new Run("\n…") { FontStyle = FontStyles.Italic });
            if (_shown++ >= ShownLines) return;
            var run = new Run((_lines.Inlines.Count > 0 ? "\n" : "") + text);
            if (text.StartsWith("CONFLICT ", StringComparison.Ordinal)) run.SetResourceReference(TextElement.ForegroundProperty, "WarningTextBrush");
            else if (text.StartsWith("NOT MERGED ", StringComparison.Ordinal))
            {
                run.SetResourceReference(TextElement.ForegroundProperty, "WarningTextBrush");
                run.FontStyle = FontStyles.Italic;
            }
            else if (text.StartsWith("ERROR", StringComparison.Ordinal)) run.SetResourceReference(TextElement.ForegroundProperty, "DangerTextBrush");
            _lines.Inlines.Add(run);
            _log.ScrollToEnd();
        }

        /// <summary>The log of an earlier merge, so the card is not empty after a restart.</summary>
        private void ShowPrevious()
        {
            try
            {
                if (!File.Exists(Merger.LogFile)) return;
                foreach (var line in File.ReadLines(Merger.LogFile).Take(ShownLines + 1)) AddLine(line.TrimEnd('\r'));
                _log.ScrollToHome();
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        private void OpenLog()
        {
            try
            {
                if (!File.Exists(Merger.LogFile)) return;
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Merger.LogFile) { UseShellExecute = true });
            }
            catch (Exception e) when (e is System.ComponentModel.Win32Exception or IOException or InvalidOperationException)
            {
                Ws.Log(L.T("Could not open the merge log: ") + e.Message, LogLevel.Warn);
            }
        }
    }
}
