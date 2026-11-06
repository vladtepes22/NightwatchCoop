using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using System.Windows;
using System.Windows.Controls;

namespace BloodborneLauncher.Views
{
    /// <summary>Screen, FPS counter, HDR and the language of the port's in-game menu.</summary>
    public sealed class DisplayPage : LauncherPage
    {
        private readonly ComboBox _monitor = new() { Width = 360, VerticalAlignment = VerticalAlignment.Center };
        private bool _refreshingMonitors;
        // Native+'s tone mapper row and the Highlight boost Native+ does not use.
        private FrameworkElement? _hdrBoostRow, _hdrToneMapperRow;

        public override void OnShown()
        {
            RefreshMonitors();
            RefreshNativePlusRows();
        }

        /// <summary>hdr_mode, Native+'s name before Native+ 0.9.38.2 (psychov30) read as nativeplus.</summary>
        private string HdrMode()
        {
            var mode = Ini.Get("hdr_mode", "native");
            return mode == "psychov30" ? "nativeplus" : mode;
        }

        private void RefreshNativePlusRows()
        {
            var nativePlus = HdrMode() == "nativeplus";
            if (_hdrBoostRow != null) _hdrBoostRow.Visibility = nativePlus ? Visibility.Collapsed : Visibility.Visible;
            if (_hdrToneMapperRow != null) _hdrToneMapperRow.Visibility = nativePlus ? Visibility.Visible : Visibility.Collapsed;
        }

        private void RefreshMonitors()
        {
            _refreshingMonitors = true;
            try
            {
                var saved = S.Str("display_device");
                _monitor.Items.Clear();
                _monitor.Items.Add(new ComboBoxItem { Content = L.T("Default (Windows primary monitor)"), Tag = "" });
                foreach (var monitor in WindowsMonitors.All())
                    _monitor.Items.Add(new ComboBoxItem { Content = monitor.Label, Tag = monitor.Device,
                        ToolTip = $"{monitor.Width} × {monitor.Height}; {monitor.Device}" });
                var selected = _monitor.Items.Cast<ComboBoxItem>().FirstOrDefault(i => string.Equals(i.Tag as string, saved, StringComparison.OrdinalIgnoreCase));
                if (selected == null && saved.Length > 0)
                {
                    selected = new ComboBoxItem { Content = L.F($"Disconnected monitor ({saved})"), Tag = saved };
                    _monitor.Items.Add(selected);
                }
                _monitor.SelectedItem = selected ?? _monitor.Items[0];
            }
            finally { _refreshingMonitors = false; }
        }

        /// <summary>Output resolution: a named size, the display's own, or Custom with its width and
        /// height (the engine takes any even WxH from 640 × 360 to 16384 × 8640).</summary>
        private void OutputResolutionRows()
        {
            static bool Named(string v) => v == "auto" || Choices.OutputResolutions.Any(c => c.Value == v && c.Value != Choices.Custom);
            var widthBox = new Wpf.Ui.Controls.TextBox { Width = 90, PlaceholderText = L.T("Width"), PlaceholderEnabled = true };
            var heightBox = new Wpf.Ui.Controls.TextBox { Width = 90, PlaceholderText = L.T("Height"), PlaceholderEnabled = true };
            var (w0, h0) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            widthBox.Text = w0.ToString();
            heightBox.Text = h0.ToString();
            SettingRow customRow = null!;
            System.Windows.Controls.TextBlock customNote = null!;

            void ApplyCustom()
            {
                if (!int.TryParse(widthBox.Text.Trim(), out var w) || !int.TryParse(heightBox.Text.Trim(), out var h) ||
                    !Choices.ValidCustomSize(w, h))
                {
                    SetHint(customNote, L.T("Width 640 to 16384, height 360 to 8640"));
                    return;
                }
                w &= ~1;
                h &= ~1;
                widthBox.Text = w.ToString();
                heightBox.Text = h.ToString();
                Ini.Set("output_res", $"{w}x{h}");
                SetHint(customNote, null);
                Ws.SaveSoon();
            }

            ComboRow(SymbolRegular.ResizeLarge24, L.T("Output resolution"), L.T("The upscaler renders below this and fills it in"),
                Choices.OutputResolutions,
                () => { var v = Ini.Get("output_res", "1920x1080"); return Named(v) ? v : Choices.Custom; },
                v =>
                {
                    customRow.Visibility = v == Choices.Custom ? Visibility.Visible : Visibility.Collapsed;
                    if (v == Choices.Custom) ApplyCustom();
                    else Ini.Set("output_res", v);
                });

            foreach (var box in new[] { widthBox, heightBox })
            {
                box.LostFocus += (_, _) => ApplyCustom();
                box.KeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) ApplyCustom(); };
            }
            customRow = Add(Card(SymbolRegular.Ruler24, L.T("Custom resolution"),
                L.T("Your own output size, any shape (5:4, 16:10, 32:9...): the camera and UI follow it with Widescreen on"),
                Row(widthBox, new System.Windows.Controls.TextBlock { Text = "×", Margin = new Thickness(8, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center }, heightBox),
                out customNote));
            customRow.Visibility = Named(Ini.Get("output_res", "1920x1080")) ? Visibility.Collapsed : Visibility.Visible;
        }

        private void IniSlider(SymbolRegular icon, string title, double min, double max, string key, Panel? into = null,
            double step = 0.05, double fallback = 1.0) =>
            SliderRow(icon, title, null, min, max, step, () => ParseDouble(Ini.Get(key, Fmt(fallback)), fallback), v => Ini.Set(key, Fmt(v)), into: into);

        public DisplayPage() : base(L.T("Display"), L.T("Resolution, window mode, FPS counter, HDR and menu language. Applied when the game starts."))
        {
            _monitor.SetResourceReference(StyleProperty, "DefaultComboBoxStyle");
            ComboSearch.Attach(_monitor);
            Group(L.T("Screen"));
            Add(Card(SymbolRegular.Desktop24, L.T("Monitor"),
                L.T("The game opens here from the start, including fullscreen and HDR. If this monitor is disconnected, the Windows primary monitor is used. Applied on the next launch."),
                _monitor, out _));
            RefreshMonitors();
            _monitor.SelectionChanged += (_, _) =>
            {
                if (_refreshingMonitors || _monitor.SelectedItem is not ComboBoxItem item) return;
                S.Set("display_device", item.Tag as string ?? "");
                Ws.SaveSoon();
            };
            OutputResolutionRows();
            SettingToggle(SymbolRegular.TextFont24, L.T("Sharp UI"),
                L.T("Menus, HUD and text drawn at the output resolution instead of 1080p stretched up: much sharper at 1440p and 4K. New: on by default in betas while it is tested. Applied on the next launch"),
                "exp_sharp_ui");
            // One choice for the window: fullscreen (bbport.ini) and borderless (launcher setting)
            // as they were, so older settings show as the mode they meant.
            ComboRow(SymbolRegular.FullScreenMaximize24, L.T("Display mode"), null, new Choice[]
                {
                    new(L.T("Windowed"), "windowed", L.T("The game in a window you can move and resize")),
                    new(L.T("Borderless"), "borderless", L.T("Fullscreen as a borderless window: trophy notifications and other windows show on top")),
                    new(L.T("Fullscreen"), "fullscreen", L.T("The driver may switch to exclusive fullscreen: lowest latency and G-SYNC's fullscreen-only mode, but windows on top are hidden")),
                },
                () => !Ini.Flag("fullscreen") ? "windowed" : S.Bool("borderless_fullscreen") ? "borderless" : "fullscreen",
                v =>
                {
                    Ini.Set("fullscreen", v == "windowed" ? "0" : "1");
                    if (v != "windowed") S.Set("borderless_fullscreen", v == "borderless");
                });
            ToggleRow(SymbolRegular.RatioOneToOne24, L.T("Widescreen"), L.T("Outputs that are not 16:9 (21:9, 32:9, 16:10, 5:4) with the right field of view and the UI kept in shape"),
                () => Ini.Get("widescreen", "1") != "0", v => Ini.Set("widescreen", v ? "1" : "0"));
            var screenMore = Advanced();
            IniCombo(SymbolRegular.ArrowSync24, L.T("Live resolution change"),
                L.T("Change the resolution in the game menu without restarting. Off is faster; turn on only if you switch resolutions in game"), Choices.LiveResolution, "live_resolution", into: screenMore);
            SettingCombo(SymbolRegular.Image24, L.T("Presentation mode"), null, Choices.PresentModes, "present_mode", into: screenMore);

            // bbport: the FPS counter (was on the Graphics page among the upscaler settings).
            Group(L.T("FPS counter"));
            IniToggle(SymbolRegular.DataUsage24, L.T("Show FPS"), L.T("Also in game: Insert or L3 + R3"), "show_fps");
            IniToggle(SymbolRegular.Wrench24, L.T("Note while shaders build"),
                L.T("\"Building shaders\" in a corner while the game builds shaders for a place it shows for the first time. Also in game: Insert or L3 + R3"),
                "compile_note");
            // Its look and contents, folded until opened.
            var fpsLook = Advanced(L.T("FPS counter settings"));
            IniCombo(SymbolRegular.PositionToFront24, L.T("FPS counter position"), null, new Choice[]
            {
                new(L.T("Top right"), "0"), new(L.T("Top left"), "1"), new(L.T("Bottom right"), "2"), new(L.T("Bottom left"), "3"), new(L.T("Top centre"), "4"),
            }, "fps_position", fpsLook);
            IniSlider(SymbolRegular.TextFontSize24, L.T("FPS counter size"), 0.75, 2.0, "fps_scale", fpsLook);
            IniCombo(SymbolRegular.Color24, L.T("FPS counter text colour"), L.T("Any colour in game: Insert or L3 + R3 › Other"), new Choice[]
            {
                new(L.T("White"), "#FFFFFF"), new(L.T("Yellow"), "#FFD54A"), new(L.T("Green"), "#7CFC8A"), new(L.T("Cyan"), "#6FE3FF"),
                new(L.T("Red"), "#FF5A5A"), new(L.T("Orange"), "#FFA040"), new(L.T("Pink"), "#FF8AD8"), new(L.T("Black"), "#000000"),
            }, "fps_text_color", fpsLook);
            IniSlider(SymbolRegular.Eye24, L.T("FPS counter text opacity"), 0.1, 1.0, "fps_text_opacity", fpsLook, fallback: 1.0);
            IniCombo(SymbolRegular.ColorBackground24, L.T("FPS counter background"), null, new Choice[]
            {
                new(L.T("Dark grey"), "#0F0F0F"), new(L.T("Black"), "#000000"), new(L.T("Dark red"), "#3A0A0A"), new(L.T("Dark blue"), "#0A1A3A"), new(L.T("White"), "#FFFFFF"),
            }, "fps_bg_color", fpsLook);
            IniSlider(SymbolRegular.Blur24, L.T("FPS counter background opacity"), 0.0, 1.0, "fps_bg_opacity", fpsLook, fallback: 0.5);
            IniToggle(SymbolRegular.BorderAll24, L.T("FPS counter border"), null, "fps_border", fpsLook);
            // What the counter shows (on unless turned off), folded again inside.
            var fpsParts = Advanced(L.T("What the FPS counter shows"), fpsLook);
            foreach (var (icon, key, title) in new[]
                     {
                         (SymbolRegular.TopSpeed24, "show_fps_rate", L.T("Frame rate")),
                         (SymbolRegular.Timer24, "show_fps_frame_time", L.T("Frame time")),
                         (SymbolRegular.ResizeLarge24, "show_fps_upscaler", L.T("Upscaler")),
                         (SymbolRegular.DeveloperBoard24, "show_fps_cpu", L.T("CPU usage")),
                         (SymbolRegular.Gauge24, "show_fps_gpu", L.T("GPU usage")),
                     })
            {
                // CPU and GPU use are off unless chosen (the engine's defaults, bbport_settings.h).
                var fallback = key is "show_fps_cpu" or "show_fps_gpu" ? "0" : "1";
                ToggleRow(icon, title, L.T("Shown in the FPS counter"), () => Ini.Get(key, fallback) == "1", v => Ini.Set(key, v ? "1" : "0"), fpsParts);
                ((System.Windows.FrameworkElement)fpsParts.Children[^1]).Margin = new System.Windows.Thickness(24, 0, 0, 0);
            }


            // The in-game map's extras (bbport.ini map_*; also in game: Insert or L3 + R3 › Other), off
            // unless chosen, as in Hunter's Atlas.
            Group(L.T("Map"));
            ToggleRow(SymbolRegular.Map24, L.T("In-game map"),
                L.T("Hold Options / Start or press M in the game. The first time, the launcher makes the map from your game files (a few minutes, in the background)"),
                () => S.Bool("map_enabled"), v => MapPrep.SetEnabled(S, v));
            var pickupKinds = new StackPanel();
            void ShowPickupKinds() => pickupKinds.Visibility = Ini.Flag("map_pickups") ? Visibility.Visible : Visibility.Collapsed;
            IniToggle(SymbolRegular.Box24, L.T("Pickups on the map"),
                L.T("Items still lying in the world, as coloured dots. Collected ones disappear; enemy drops and hidden items are not shown"),
                "map_pickups", changed: ShowPickupKinds);
            Body.Children.Add(pickupKinds);
            foreach (var (key, title) in new[]
                     {
                         ("map_pickups_healing", L.T("Blood Vials (red)")),
                         ("map_pickups_insight", L.T("Insight items (purple)")),
                         ("map_pickups_equipment", L.T("Weapons and attire (blue)")),
                         ("map_pickups_coldblood", L.T("Coldblood (gold)")),
                     })
            {
                ToggleRow(SymbolRegular.Circle24, title, null, () => Ini.Get(key, "1") == "1", v => Ini.Set(key, v ? "1" : "0"), pickupKinds);
                ((System.Windows.FrameworkElement)pickupKinds.Children[^1]).Margin = new System.Windows.Thickness(24, 0, 0, 0);
            }
            ShowPickupKinds();
            IniToggle(SymbolRegular.EyeOff24, L.T("Boss reveal"),
                L.T("An area's map stays hidden until its boss is dead (lamps you have lit always show)"), "map_boss_reveal");
            ToggleRow(SymbolRegular.Location24, L.T("Map travel"),
                L.T("Select a lit lamp on the map and press again to travel there (not in boss fights, Chalice Dungeons or cutscenes)"),
                () => Ini.Get("map_travel", "1") == "1", v => Ini.Set("map_travel", v ? "1" : "0"));
            ComboRow(SymbolRegular.ChatMultiple24, L.T("NPC conversation sites"),
                L.T("New dialogue only covers a few early conversations so far (Gilbert, Eileen's first ones)"),
                new Choice[] { new(L.T("All"), "all"), new(L.T("New dialogue only"), "new_dialogue"), new(L.T("None"), "none") },
                () => Ini.Get("map_npcs", "all"), v => Ini.Set("map_npcs", v));

            Group("HDR");
            var hdrHint = WindowsHdr.AnyOn()
                ? L.T("The game's picture on an HDR screen, with brighter highlights. Applied when the game starts")
                : WindowsHdr.AnySupported()
                    ? L.T("Turn on Use HDR in Windows' display settings first (Settings › System › Display)")
                    : L.T("This display shows no HDR");
            // The HDR settings show only while HDR output is on.
            var hdrRows = new StackPanel();
            void ShowHdrRows() => hdrRows.Visibility = Ini.Flag("hdr") ? Visibility.Visible : Visibility.Collapsed;
            IniToggle(SymbolRegular.Hdr24, L.T("HDR output"), hdrHint, "hdr", changed: ShowHdrRows);
            Body.Children.Add(hdrRows);
            ShowHdrRows();
            ComboRow(SymbolRegular.Lightbulb24, L.T("HDR source"),
                "Native: highlights as bright as they are in the game's scene. Expanded: the SDR picture's bright parts made brighter. Native+: the light of the game's scene kept to the screen, tone mapped to your display's peak by Vanilla+ or PsychoV (its settings are in the in-game menu too); entering or leaving it needs a game restart",
                Choices.HdrMode, HdrMode, v => Ini.Set("hdr_mode", v), hdrRows,
                changed: RefreshNativePlusRows);
            SliderRow(SymbolRegular.BrightnessHigh24, L.T("Paper white"), L.T("How bright the game's white is, in nits. Range: 10–500"),
                10, 500, 1, () => ParseDouble(Ini.Get("hdr_paper_white", "200"), 200),
                v => Ini.Set("hdr_paper_white", v.ToString("0", System.Globalization.CultureInfo.InvariantCulture)),
                format: L.T("0' nits'"), into: hdrRows, valueWidth: 82);
            ToggleRow(SymbolRegular.Desktop24, L.T("Peak brightness from the display"),
                L.T("The peak Windows reports for the display the game is on (the Windows HDR Calibration app's, once it has run). Off: the value below"),
                () => Ini.Get("hdr_peak_auto", "1") != "0", v => Ini.Set("hdr_peak_auto", v ? "1" : "0"), hdrRows);
            SliderRow(SymbolRegular.WeatherSunny24, L.T("Peak brightness"), L.T("Your display's peak brightness, in nits, when it is not taken from the display. Range: 10–4000"),
                10, 4000, 10, () => ParseDouble(Ini.Get("hdr_peak", "1000"), 1000),
                v => Ini.Set("hdr_peak", v.ToString("0", System.Globalization.CultureInfo.InvariantCulture)),
                format: L.T("0' nits'"), into: hdrRows, valueWidth: 82);
            // The rows themselves (ComboRow returns its drop-down): the last one added to hdrRows.
            FrameworkElement LastHdrRow() => (FrameworkElement)hdrRows.Children[hdrRows.Children.Count - 1];
            ComboRow(SymbolRegular.Sparkle24, L.T("Highlight boost"), L.T("Off keeps the game's own look; higher makes lamps, fire and the sky brighter"),
                Choices.HdrBoost, () => Ini.Get("hdr_boost", "0.25"), v => Ini.Set("hdr_boost", v), hdrRows);
            _hdrBoostRow = LastHdrRow();
            ComboRow(SymbolRegular.Lightbulb24, L.T("Tone mapper"),
                "Native+ only. Vanilla+ (by souperman9, from his RenoDX Bloodborne mod): the game's image up to mid grey, brighter highlights up to your display's peak. PsychoV: RenoDX's PsychoV-30 with Vanilla+'s bloom; deeper blacks, softer highlights with the most detail near the peak. Its Vanilla+ Highlights and Hue Correction (in-game menu) take it towards Vanilla+'s brightness and hues. Also in the in-game menu, live",
                Choices.HdrToneMapper, () => Choices.HdrToneMapperValue(Ini.Get("hdr_tonemapper", "5")), v => Ini.Set("hdr_tonemapper", v), hdrRows);
            _hdrToneMapperRow = LastHdrRow();
            RefreshNativePlusRows();
            var hdrMore = Advanced(into: hdrRows);
            ComboRow(SymbolRegular.TextFont24, L.T("UI brightness"), L.T("Native source only: how bright the HUD, menus and subtitles are, against paper white"),
                Choices.HdrUiBrightness, () => Ini.Get("hdr_ui_brightness", "1"), v => Ini.Set("hdr_ui_brightness", v), hdrMore);
            ComboRow(SymbolRegular.Color24, L.T("UI colour"), L.T("Native source only: Vivid shows the HUD and menus with the colours a wide-gamut monitor gives SDR; Exact keeps sRGB's"),
                Choices.HdrUiColor, () => Ini.Get("hdr_ui_color", "1"), v => Ini.Set("hdr_ui_color", v), hdrMore);

            Group(L.T("Language"));
            ComboRow(SymbolRegular.LocalLanguage24, L.T("In-game menu language"), L.T("The port's settings menu (Insert or L3 + R3)"),
                Choices.MenuLanguages, () => Ini.Get("menu_language", "en"), v => Ini.Set("menu_language", v));
            SettingCombo(SymbolRegular.Translate24, L.T("Game language"), L.T("The game's own text and voices"), Choices.GameLanguages, "language");
            SettingCombo(SymbolRegular.Keyboard24, L.T("In-game menu shortcut"),
                L.T("What opens the port's settings menu while playing. Applied on the next launch"), Choices.OverlayKeys, "overlay_keys");
        }
    }
}
