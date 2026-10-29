using System.IO;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// bbport.ini: the port's own settings (upscaler, preset, controls, ...), also edited by the
    /// in-game menu. The edited keys are rewritten in place, missing ones
    /// appended, comments and unknown keys kept, and a key set to null is removed (a control
    /// binding back to its default).
    /// </summary>
    public sealed class IniFile
    {
        public static readonly (string Key, string Value)[] Defaults =
        {
            ("upscaler", OperatingSystem.IsWindows() ? "fsr" : "fsr4"),
            ("preset", "4"),
            ("sharpen", "1"),
            ("sharpness", "0.50"),
            ("object_motion", "1"),
            ("show_fps", "1"),
            ("compile_note", "1"),
            ("output_res", "1920x1080"),
            ("model_lod", "0"),
            ("camera_tuning", "original"),
            ("live_resolution", "0"),
            ("frame_gen", "off"),
            ("frame_gen_cap", "1"),
            ("dlss_preset", "k"),
            ("fps_position", "0"),
            ("fps_scale", "1.00"),
            ("fps_text_color", "#FFFFFF"),
            ("fps_bg_color", "#0F0F0F"),
            ("fps_bg_opacity", "0.50"),
            ("fps_text_opacity", "1.00"),
            ("fps_border", "1"),
        };

        private readonly Dictionary<string, string?> _values = new(StringComparer.Ordinal);
        private List<string> _lines = new();
        /// <summary>Keys the launcher changed since the file was read: only these are written over it.
        /// The game edits the same file (in-game menu) while the launcher holds an older copy.</summary>
        private readonly HashSet<string> _changed = new(StringComparer.Ordinal);

        public void Load()
        {
            _changed.Clear();
            _values.Clear();
            foreach (var (key, value) in Defaults)
                _values[key] = value;
            foreach (var effect in Choices.Effects)
                _values[effect.Key] = effect.Default ? "1" : "0";

            _lines = new List<string>();
            try
            {
                if (File.Exists(Paths.IniPath))
                    _lines = File.ReadAllLines(Paths.IniPath).ToList();
            }
            catch
            {
                // Unreadable: start from the defaults.
            }

            foreach (var line in _lines)
            {
                if (!line.Contains('=') || line.TrimStart().StartsWith('#'))
                    continue;
                var at = line.IndexOf('=');
                _values[line[..at].Trim()] = line[(at + 1)..].Trim();
            }
        }

        public void Save()
        {
            // What the game wrote meanwhile stays: the file is read again and only the launcher's own
            // changes go over it (PC Enhancements set in the game came back off after it exited).
            var mine = _changed.ToDictionary(k => k, k => _values.TryGetValue(k, out var v) ? v : null, StringComparer.Ordinal);
            Load();
            foreach (var (key, value) in mine)
            {
                _values[key] = value;
                _changed.Add(key); // Load() cleared it; stays pending until the write below succeeds
            }
            var written = new HashSet<string>(StringComparer.Ordinal);
            var output = new List<string>();
            foreach (var line in _lines)
            {
                if (line.Contains('=') && !line.TrimStart().StartsWith('#'))
                {
                    var key = line[..line.IndexOf('=')].Trim();
                    if (_values.TryGetValue(key, out var value))
                    {
                        if (value != null && written.Add(key))
                            output.Add($"{key}={value}");
                        written.Add(key);
                        continue;
                    }
                }
                output.Add(line);
            }

            if (_lines.Count == 0)
                output.Add("# bbport settings (in-game menu: Insert / L3+R3)");
            foreach (var (key, value) in _values)
                if (!written.Contains(key) && value != null)
                    output.Add($"{key}={value}");

            Directory.CreateDirectory(Path.GetDirectoryName(Paths.IniPath)!);
            File.WriteAllText(Paths.IniPath + ".tmp", string.Join("\n", output) + "\n");
            File.Move(Paths.IniPath + ".tmp", Paths.IniPath, true);
            _lines = output;
            _changed.Clear();
        }

        /// <summary>The value, or null when the key is absent (e.g. a binding left at its default).</summary>
        public string? Raw(string key) => _values.TryGetValue(key, out var v) ? v : null;

        public string Get(string key, string fallback = "") => Raw(key) ?? fallback;

        public bool Flag(string key) => Get(key) == "1";

        public void Set(string key, string? value)
        {
            if (_values.TryGetValue(key, out var old) && old == value) return;
            _values[key] = value;
            _changed.Add(key);
        }

        /// <summary>bbport.ini's one-time changes done so far (the game's kSettingsRev,
        /// bbport_settings.h: whichever starts first does them). 1 (visy 0.7.2): party_rot=0 once,
        /// rot became off by default and older games saved their default 1 with every change.
        /// 2: model_lod -2 -> 0 once, Highest came back as an opt-in and older files still hold the
        /// -2 that 0.2.1 beta 1 read as 0. 3 (0.2.5): change_appearance=1 and rebirth=1 once, the Dream
        /// mirror and Rebirth at the Altar of Despair became on by default.</summary>
        public const int SettingsRev = 3;

        /// <summary>Applies the one-time changes a bbport.ini below <see cref="SettingsRev"/> still
        /// needs; a new file only gets the revision.</summary>
        public void MigrateRev()
        {
            if (!int.TryParse(Get("settings_rev", "0"), out var rev)) rev = 0;
            if (rev >= SettingsRev) return;
            if (File.Exists(Paths.IniPath))
            {
                if (rev < 1) Set("party_rot", "0");
                if (rev < 2 && Get("model_lod", "0") == "-2") Set("model_lod", "0");
                if (rev < 3) { Set("change_appearance", "1"); Set("rebirth", "1"); }
                Set("settings_rev", SettingsRev.ToString());
                Save();
            }
            else
                Set("settings_rev", SettingsRev.ToString());
        }

        /// <summary>Every key with a value (shared settings).</summary>
        public IEnumerable<KeyValuePair<string, string>> Values =>
            _values.Where(kv => kv.Value != null).Select(kv => new KeyValuePair<string, string>(kv.Key, kv.Value!));
    }
}
