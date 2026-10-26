using System.IO;
using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBox = System.Windows.Controls.TextBox;

namespace BloodborneLauncher.Views
{
    /// <summary>The game's own output (run.py and bb-probe), live while it runs.</summary>
    public sealed class OutputPage : LauncherPage
    {
        private readonly TextBox _output;
        private readonly ToggleSwitch _follow;
        private readonly Wpf.Ui.Controls.Button _copy;

        public OutputPage() : base(L.T("Game output"), L.T("What run.py and the game print. Copy or export it when reporting a problem."))
        {
            var bar = new Grid { Margin = new Thickness(0, 18, 0, 8) };
            bar.ColumnDefinitions.Add(new ColumnDefinition());
            bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            _follow = new ToggleSwitch { IsChecked = true, Content = L.T("Follow new output"), VerticalAlignment = VerticalAlignment.Center };
            bar.Children.Add(_follow);

            // The log keeps growing while the game runs: Copy waits for the session to end.
            _copy = SmallButton(L.T("Copy"), SymbolRegular.Copy24, (_, _) => Copy());
            _copy.IsEnabled = !Ws.Game.Running;
            Ws.GameStateChanged += () => _copy.IsEnabled = !Ws.Game.Running;
            var buttons = Row(
                _copy,
                SmallButton(L.T("Export…"), SymbolRegular.Save24, (_, _) => Export()),
                // The logs folder: last / previous session and the earlier ones in logs\sessions.
                SmallButton(L.T("Open"), SymbolRegular.FolderOpen24, (_, _) => OpenLogs()),
                SmallButton(L.T("Clear"), SymbolRegular.Broom24, (_, _) => Ws.ClearOutput()));
            Grid.SetColumn(buttons, 1);
            bar.Children.Add(buttons);
            Body.Children.Add(bar);

            SettingToggle(SymbolRegular.WindowConsole20, L.T("Show this page when the game starts"), null, "switch_to_output");

            _output = new TextBox { Height = 520, Margin = new Thickness(0, 4, 0, 0) };
            _output.SetResourceReference(StyleProperty, "CodeBox");
            Body.Children.Add(_output);

            // One append per batch; while the page is hidden nothing is laid out, and it catches up
            // in one go when shown.
            Ws.GameOutput += batch =>
            {
                if (!IsVisible)
                {
                    _stale = true;
                    return;
                }
                _lines += batch.Count;
                if (_lines > Workspace.MaxOutputLines + 500)
                {
                    Rebuild();
                    return;
                }
                _output.AppendText(string.Join("\n", batch) + "\n");
                if (_follow.IsChecked == true)
                    _output.ScrollToEnd();
            };
            Ws.OutputCleared += () =>
            {
                _output.Clear();
                _lines = 0;
            };
            IsVisibleChanged += (_, _) => { if (IsVisible && _stale) Rebuild(); };
            Rebuild();
        }

        private int _lines;
        private bool _stale;

        private void Rebuild()
        {
            _output.Text = Ws.OutputLines.Count > 0 ? string.Join("\n", Ws.OutputLines) + "\n" : "";
            _lines = Ws.OutputLines.Count;
            _stale = false;
            if (_follow.IsChecked == true)
                _output.ScrollToEnd();
        }

        public override void OnShown()
        {
            if (_stale) Rebuild();
            else if (_follow.IsChecked == true)
                _output.ScrollToEnd();
        }

        private async void Copy()
        {
            // The whole session log, not the last lines this page keeps (same source as Export). Read
            // off the UI thread so a big log doesn't freeze the launcher.
            // Addresses, user folder, player name and passwords hidden, as in the logs menu's copies.
            string text;
            var settings = S;
            try
            {
                text = await Task.Run(() =>
                {
                    if (!File.Exists(GameRunner.SessionLog)) return LogCopy.Redact(_output.Dispatcher.Invoke(() => _output.Text), settings);
                    using var from = new FileStream(GameRunner.SessionLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    using var reader = new StreamReader(from);
                    return LogCopy.Redact(reader.ReadToEnd(), settings);
                });
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not read the log: ") + e.Message, LogLevel.Warn);
                return;
            }
            if (text.Length == 0) return;
            if (ClipboardText.Set(text))
                Ws.Log(L.T("Game output copied."), LogLevel.Good);
            else
                Ws.Log(L.T("Could not copy: another program is using the clipboard. Try again, or use Export."), LogLevel.Warn);
        }

        private void OpenLogs()
        {
            try
            {
                var folder = Path.GetDirectoryName(GameRunner.SessionLog)!;
                Directory.CreateDirectory(folder);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", folder) { UseShellExecute = true });
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not open the folder: ") + e.Message, LogLevel.Warn);
            }
        }

        private void Export()
        {
            var dialog = new Microsoft.Win32.SaveFileDialog
            {
                Title = L.T("Export the game output"),
                FileName = $"nightwatch-{DateTime.Now:yyyyMMdd_HHmmss}.log",
                Filter = L.T("Log files|*.log|Text files|*.txt|All files|*.*"),
            };
            if (dialog.ShowDialog() != true) return;
            try
            {
                // The whole session (logs\last_session.log), not the last lines this page keeps: a
                // report trimmed to its end lost the start and what came before a freeze.
                if (File.Exists(GameRunner.SessionLog))
                {
                    using var from = new FileStream(GameRunner.SessionLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    using var to = File.Create(dialog.FileName);
                    from.CopyTo(to);
                }
                else
                    File.WriteAllText(dialog.FileName, _output.Text);
                Ws.Log(L.T("Game output saved: ") + dialog.FileName, LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not save the output: ") + e.Message, LogLevel.Bad);
            }
        }
    }
}
