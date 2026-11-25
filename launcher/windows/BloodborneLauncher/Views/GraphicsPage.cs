using System.IO;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>Upscaling and the game's own effects (patches applied at start).</summary>
    public sealed class GraphicsPage : LauncherPage
    {
        private readonly TextBlock _upscalerHint, _dlssHint, _frameGenHint;
        private TextBlock? _motionBlurHint;
        private readonly List<(ToggleSwitch Toggle, TextBlock Hint, string? Text)> _coopOff = new();
        private readonly SettingRow _glowSizeCard;
        private readonly SettingRow _presetCard, _renderScaleCard, _sharpenCard, _sharpnessCard, _mipBiasCard;
        private readonly SettingRow _frameGenFramesCard, _frameGenDynamicCard, _reflexCard, _frameGenCapCard;

        public GraphicsPage() : base(L.T("Graphics"),
            L.T("Stored in bbport.ini; the upscaler can also be changed in game (Insert or L3 + R3)."))
        {
            // Presets (Views/PresetsWindow.cs) fill in the settings below (picture, effects, lighting); they stay editable.
            Add(Card(SymbolRegular.Gauge24, L.T("Best settings for this PC"), Hardware.Detect().Summary,
                Row(SmallButton(L.T("Recommend…"), SymbolRegular.Sparkle24, (_, _) => MainWindow.Instance!.OfferBestSettings(firstRun: false)),
                    SmallButton(L.T("Presets…"), SymbolRegular.Options24, (_, _) => PresetsWindow.Open())), out _));

            Group(L.T("Upscaler"));
            var fsr411 = Fsr4VkFound() && !Amd();
            if (Ini.Get("upscaler") is "fsr3" or "fsr4" || (!fsr411 && Ini.Get("upscaler") == "fsr411")) Ini.Set("upscaler", "fsr");
            IniCombo(SymbolRegular.Sparkle24, L.T("Method"), null,
                fsr411 ? Choices.Upscalers : Choices.Upscalers.Where(c => c.Value != "fsr411").ToArray(), "upscaler", changed: UpdateUpscaler);
            _upscalerHint = (TextBlock)((SettingRow)Body.Children[^1]).Tag;

            IniCombo(SymbolRegular.ZoomIn24, L.T("Quality preset"), L.T("How far below the output resolution the game renders"),
                Choices.Presets, "preset", changed: UpdateUpscaler);
            _presetCard = (SettingRow)Body.Children[^1];
            // The Custom preset's ratio (render_scale), the engine's PresetScale: 1.0 renders at the
            // output (DLAA with DLSS), 3.0 is Ultra Performance.
            SliderRow(SymbolRegular.SlideSize24, L.T("Render scale (Custom preset)"),
                L.T("Output size divided by this is the render size: 1.00 renders at the output, 1.30 a little below, 3.00 like Ultra Performance. FSR 4 and DLSS use the nearest preset's model"),
                1.0, 3.0, 0.05, () => ParseDouble(Ini.Get("render_scale", "1.30"), 1.3), v => Ini.Set("render_scale", Fmt(v)), format: "x0.00", valueWidth: 56);
            _renderScaleCard = (SettingRow)Body.Children[^1];

            IniToggle(SymbolRegular.Eyedropper24, L.T("Sharpening (RCAS)"), L.T("Works with every option above, TAA included: turn it up if the picture looks soft"), "sharpen");
            _sharpenCard = (SettingRow)Body.Children[^1];
            SliderRow(SymbolRegular.SlideSize24, L.T("Sharpening strength"), L.T("Up to 1: the upscaler's own sharpening. Above 1: an extra pass"), 0, 2, 0.05,
                () => ParseDouble(Ini.Get("sharpness"), 0.5), v => Ini.Set("sharpness", Fmt(v)));
            _sharpnessCard = (SettingRow)Body.Children[^1];
            SliderRow(SymbolRegular.Image24, L.T("Texture mip bias"),
                L.T("Added to the texture detail the upscaler asks for: below 0 sharper textures (more shimmer), above 0 softer. 0 is the upscaler's own"),
                -2, 1, 0.05, () => ParseDouble(Ini.Get("mip_bias", "0"), 0), v => Ini.Set("mip_bias", Fmt(v)), format: "+0.00;-0.00;0.00", valueWidth: 56);
            _mipBiasCard = (SettingRow)Body.Children[^1];
            IniToggle(SymbolRegular.ArrowMove24, L.T("Object motion vectors"), L.T("Less ghosting on characters; costs about 10% FPS"), "object_motion");

            var dlssMore = Advanced(L.T("DLSS options"));
            IniCombo(SymbolRegular.BrainCircuit24, L.T("DLSS model"),
                L.T("Which NVIDIA model DLSS runs. Also in game (Insert or L3 + R3), where it changes at once"), Choices.DlssModels, "dlss_preset", dlssMore);
            ToggleRow(SymbolRegular.BrightnessHigh24, L.T("DLSS auto exposure"),
                L.T("DLSS works out each frame's exposure itself, for the game's finished frame too. Can calm ghosting in dark scenes"),
                () => Ini.Get("dlss_auto_exposure", "0") == "1", v => Ini.Set("dlss_auto_exposure", v ? "1" : "0"), dlssMore);
            ToggleRow(SymbolRegular.Image24, L.T("DLSS sharper textures"),
                L.T("One more mip level of texture detail under DLSS, as NVIDIA's guide asks. Off: the render-to-output ratio only"),
                () => Ini.Get("dlss_mip_bias", "1") != "0", v => Ini.Set("dlss_mip_bias", v ? "1" : "0"), dlssMore);
            Add(Card(SymbolRegular.DeveloperBoard24, "DLSS", "", null, out _dlssHint), dlssMore);

            // Moved here from the Display page: the frame rate sits beside frame generation.
            Group(L.T("Frame rate"));
            SettingCombo(SymbolRegular.Gauge24, L.T("Frame rate"), L.T("Nightwatch Co-op runs the game at the rate you pick and keeps its timing right there, also when a scene runs slower. 30 FPS is the game's own"), Choices.FpsModes, "fps_mode", changed: UpdateUpscaler);

            IniToggle(SymbolRegular.Clock24, L.T("Live FPS clock"),
                L.T("The game steps by each frame's measured time (Mrsuss60's clock, from visy's 0.7.8) instead of the fixed frame-rate code: cloth, animation and moving platforms follow real time. On by default; not used at 30 FPS. The target can be changed in game (Insert menu, Live FPS)"), "live_fps");
            Group(L.T("Frame generation"));
            IniCombo(SymbolRegular.FastForward24, L.T("Method"),
                L.T("Extra frames between the game's own: smoother motion, a little more input lag. Needs an upscaler. Off by default"),
                Choices.FrameGeneration, "frame_gen", changed: UpdateUpscaler);
            _frameGenHint = (TextBlock)((SettingRow)Body.Children[^1]).Tag;
            IniCombo(SymbolRegular.Layer24, L.T("DLSS generated frames"),
                L.T("Frames shown per frame the game renders. More than 2x needs an RTX 50 GPU; the engine keeps to what the GPU allows"),
                Choices.FrameGenFrames, "frame_gen_frames");
            _frameGenFramesCard = (SettingRow)Body.Children[^1];
            IniToggle(SymbolRegular.TopSpeed24, L.T("DLSS dynamic frame generation"),
                L.T("RTX 50: the driver picks 2x, 3x or 4x moment by moment to reach your refresh rate, up to the generated frames above. Fewer generated frames when the game is fast: less lag, fewer artifacts. Also in game, with a target FPS"), "frame_gen_dynamic");
            _frameGenDynamicCard = (SettingRow)Body.Children[^1];
            IniCombo(SymbolRegular.Flash24, L.T("NVIDIA Reflex"),
                L.T("Keeps the GPU's queue short for lower input lag. Always on while DLSS Frame Generation runs"),
                Choices.Reflex, "reflex");
            _reflexCard = (SettingRow)Body.Children[^1];
            IniToggle(SymbolRegular.Timer24, L.T("Frame generation: even pacing (Uncapped)"),
                L.T("With the Uncapped frame rate: the game at the refresh rate divided by the frames shown per game frame, so every generated frame reaches the screen. The fixed rates (30 to 144) are left as they are"), "frame_gen_cap");
            _frameGenCapCard = (SettingRow)Body.Children[^1];

            // The engine's picture and camera controls (bbport_settings.cpp clamps the same ranges).
            Group(L.T("Picture"));
            var picture = new StackPanel();
            IniToggle(SymbolRegular.Color24, L.T("Picture tuning"), L.T("Turns the settings below on, live. Also in the game's System menu (PC Graphics, PC Effects, PC Colour) and the Insert menu"), "graphics_controls");
            IniToggle(SymbolRegular.CircleHalfFill24, L.T("Vignette"), null, "graphics_vignette", picture, on: true); // engine default on
            Body.Children.Add(picture);
            // The sliders, folded until opened.
            var adjustments = Advanced(L.T("Picture adjustments"));
            IniSlider(SymbolRegular.DarkTheme24, L.T("Ambient occlusion strength"), 0, 3, "graphics_ao_strength", adjustments);
            IniSlider(SymbolRegular.WeatherSunny24, L.T("Shadow distance"), 0.5, 4, "graphics_shadow_scale", adjustments);
            IniSlider(SymbolRegular.Lightbulb24, L.T("Bloom"), 0, 3, "graphics_bloom", adjustments);
            IniSlider(SymbolRegular.Color24, L.T("Saturation"), 0, 3, "graphics_saturation", adjustments);
            IniSlider(SymbolRegular.BrightnessHigh24, L.T("Brightness"), 0, 3, "graphics_brightness", adjustments);
            IniSlider(SymbolRegular.DarkTheme24, L.T("Contrast"), 0.8, 1.2, "graphics_contrast", adjustments, step: 0.01);
            IniSlider(SymbolRegular.Color24, L.T("Hue"), -180, 180, "graphics_hue", adjustments, step: 1, fallback: 0);
            IniSlider(SymbolRegular.WeatherFog24, L.T("Fog"), 0, 3, "graphics_fog", adjustments);

            Group(L.T("Camera"));
            IniToggle(SymbolRegular.Camera24, L.T("Camera tuning"), L.T("Turns the three settings below on"), "camera_controls");
            IniSlider(SymbolRegular.ZoomOut24, L.T("Field of view"), 1, 1.5, "camera_fov_scale");
            IniSlider(SymbolRegular.ArrowMaximize24, L.T("Distance"), 0.5, 1.5, "camera_distance_scale");
            IniSlider(SymbolRegular.ArrowBidirectionalUpDown24, L.T("Height"), 0.5, 1.5, "camera_height_scale");

            Group(L.T("Patches"));
            Body.Children.Add(Text(L.T("Patches to the game, applied when it starts."), "Faint"));
            ComboRow(SymbolRegular.Cube24, L.T("Model detail"), null, Choices.ModelLod,
                () => Choices.ModelLodShown(Ini.Get("model_lod")), v => Ini.Set("model_lod", v));
            IniCombo(SymbolRegular.WeatherSunny24, L.T("Sun shadow resolution"), L.T("Higher needs more video memory"),
                Choices.SunShadows, "graphics_shadow_resolution");
            IniCombo(SymbolRegular.Lightbulb24, L.T("Lamp shadow resolution"), L.T("Higher needs more video memory"),
                Choices.LampShadows, "graphics_local_shadow_resolution");
            IniCombo(SymbolRegular.Blur24, L.T("Motion blur quality"), L.T("Smoother motion blur; motion blur must be on"),
                Choices.MotionBlurQuality, "graphics_motion_blur_quality");
            IniCombo(SymbolRegular.Camera24, L.T("Follow camera"), L.T("Original keeps the game's camera distance, angle and follow speed at 60 FPS and above"), Choices.CameraTuning, "camera_tuning");
            foreach (var effect in Choices.Effects.Take(Choices.FirstLightingEffect))
            {
                var motionBlur = effect.Key == "effect_motion_blur";
                var toggle = IniToggle(EffectIcon(effect.Key), effect.Title, effect.Hint, effect.Key, changed: motionBlur ? UpdateUpscaler : null, on: effect.Default);
                if (motionBlur) _motionBlurHint = ((SettingRow)Body.Children[^1]).Hint;
                if (effect.Key is "debug_camera" or "debug_menu")
                    _coopOff.Add((toggle, ((SettingRow)Body.Children[^1]).Hint, effect.Hint));
            }

            // Optional lighting: all off, needs an upscaler on (bbport_settings.h EffectFirstLighting).
            Group(L.T("Lighting"));
            Body.Children.Add(Text(L.T("Optional lighting passes; all off by default. They need an upscaler on"), "Faint"));
            foreach (var effect in Choices.Effects.Skip(Choices.FirstLightingEffect))
            {
                IniToggle(SymbolRegular.Lightbulb24, effect.Title, effect.Hint, effect.Key, on: effect.Default);
                if (effect.Key == "effect_gtao")
                    IniCombo(SymbolRegular.Lightbulb24, L.T("Ambient occlusion quality"),
                        Choices.Hint(L.T("High looks sharper and steadier but costs more GPU time"),
                            L.T("Needs Better ambient occlusion (GTAO) on"), L.T("Applies after a restart")),
                        Choices.AoQualities, "graphics_ao_quality");
                if (effect.Key == "effect_gtao")
                    IniToggle(SymbolRegular.Lightbulb24, L.T("Smooth normals for AO and indirect light"), Choices.SmoothNormalsHint,
                        "graphics_smooth_normals");
            }
            IniSlider(SymbolRegular.Lightbulb24, L.T("Indirect lighting strength"), 0, 12, "graphics_gi_strength", fallback: 6);
            IniSlider(SymbolRegular.Lightbulb24, L.T("Reflection strength"), 0, 3, "graphics_ssr_intensity");
            IniSlider(SymbolRegular.Lightbulb24, L.T("Volumetric sunlight strength"), 0, 3, "graphics_volumetric_intensity", fallback: 0.2);
            IniSlider(SymbolRegular.Lightbulb24, L.T("Lamp fog strength"), 0, 3, "graphics_point_fog_intensity", fallback: 0.2);
            ComboRow(SymbolRegular.Lightbulb24, L.T("Lamp fog falloff"),
                L.T("Classic is the original lamp fog. Inverse-square gives a bright, tight glow at each flame that falls off fast, and costs more GPU time than classic. Needs Fog around lamps."),
                Choices.PointFogFalloff, () => Ini.Get("graphics_point_fog_falloff", "0"), v => Ini.Set("graphics_point_fog_falloff", v),
                changed: UpdateGlowSize);
            SliderRow(SymbolRegular.Lightbulb24, L.T("Lamp fog glow size"), L.T("Used by the inverse-square falloff"), 0.03, 0.2, 0.01,
                () => ParseDouble(Ini.Get("graphics_point_fog_glow_size", Fmt(0.08)), 0.08), v => Ini.Set("graphics_point_fog_glow_size", Fmt(v)));
            _glowSizeCard = (SettingRow)Body.Children[^1];
            UpdateGlowSize();

            Ws.PortChanged += UpdateUpscaler;
        }

        private void IniSlider(SymbolRegular icon, string title, double min, double max, string key, Panel? into = null,
            double step = 0.05, double fallback = 1.0) =>
            SliderRow(icon, title, null, min, max, step, () => ParseDouble(Ini.Get(key, Fmt(fallback)), fallback), v => Ini.Set(key, Fmt(v)), into: into);

        /// <summary>An RTX 40 or 50 card (desktop or laptop): the GPUs NVIDIA's frame generation runs on.</summary>
        private static bool DlssFrameGenGpu() =>
            Hardware.Detect() is { Rtx: true } hardware &&
            System.Text.RegularExpressions.Regex.IsMatch(hardware.Gpu, @"RTX\s*(PRO\s*)?[45]0\d\d", System.Text.RegularExpressions.RegexOptions.IgnoreCase);

        /// <summary>NVIDIA Streamline's runtime beside the engine (tools/fetch_streamline.sh): DLSS Frame Generation.</summary>
        private static bool StreamlineFound() =>
            File.Exists(Path.Combine(Paths.BinariesDir, "sl.interposer.dll")) && File.Exists(Path.Combine(Paths.BinariesDir, "sl.dlss_g.dll"));

        private static SymbolRegular EffectIcon(string key) => key switch
        {
            "skip_intro" => SymbolRegular.FastForward24,
            "debug_camera" => SymbolRegular.Camera24,
            "debug_menu" => SymbolRegular.Bug24,
            _ => SymbolRegular.Color24,
        };

        private void UpdateGlowSize() => _glowSizeCard.IsEnabled = Ini.Get("graphics_point_fog_falloff", "0") == "1";

        public override void OnShown() { UpdateUpscaler(); UpdateGlowSize(); UpdateCoopOff(); }

        // Free camera and the debug menu are game patches that count in the party patch hash: in co-op every
        // player needs the same setting (a join with another one is refused naming them), and the rows say so.
        private void UpdateCoopOff()
        {
            var coop = SeamlessSettings.Enabled(S);
            foreach (var (_, hint, text) in _coopOff)
                SetHint(hint, coop ? L.T("Co-op: every player in the party needs the same setting, else joins are refused") : text);
        }

        private static bool DlssFound() =>
            Paths.DlssRuntime != null; // the engine has DLSS built in; NVIDIA's runtime sits beside it

        private void UpdateUpscaler()
        {
            var value = Ini.Get("upscaler");
            string? hint = value switch
            {
                "fsr" when !FsrSdkFound() =>
                    L.T("fsr_sdk is missing from the engine folder: the game falls back to FSR 3.1"),
                "fsr" when Amd() =>
                    L.T("AMD FSR SDK 2.4: FSR 4.1.2 on Radeon RX 6000 and newer and on supported Ryzen APUs, FSR 3.1.6 on older Radeons. The SDK picks the model"),
                "fsr" =>
                    L.T("AMD FSR SDK 2.4: FSR 3.1.6 on this GPU (AMD's FSR 4 runs on Radeon only). FSR 4.1.1 (fsr4vk, experimental) is the other choice here"),
                "fsr411" =>
                    L.T("Through fsr4vk (included, experimental): FSR 4.1.1 on NVIDIA and Intel GPUs with the needed Vulkan features. Choose FSR if it looks wrong"),
                "dlss" => DlssFound()
                    ? L.T("Needs an NVIDIA GeForce RTX GPU; others fall back to FSR 3.1") +
                      (Ini.Get("preset", "0") == "0" ? L.T(". With Native AA this is DLAA: DLSS as anti-aliasing at full resolution") : "")
                    : L.T("DLSS files are missing: the game falls back to FSR 3.1"),
                "taa" => L.T("Anti-aliasing at the output resolution, without an FSR model. Softer than DLAA or FSR Native AA: add sharpening below"),
                _ => null,
            };
            SetHint(_upscalerHint, hint);
            _presetCard.IsEnabled = value is not ("taa" or "off");
            _renderScaleCard.Visibility = Ini.Get("preset", "0") == "5" ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
            _renderScaleCard.IsEnabled = value is not ("taa" or "off");
            _sharpenCard.IsEnabled = value != "off";
            _sharpnessCard.IsEnabled = value != "off";
            _mipBiasCard.IsEnabled = value != "off";

            // Frame generation: only the rows that apply to the chosen kind (DLSS's own with DLSS,
            // even pacing with either, nothing when it is off).
            var frameGen = Ini.Get("frame_gen", "off");
            static System.Windows.Visibility Shown(bool on) => on ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
            _frameGenFramesCard.Visibility = Shown(frameGen == "dlss");
            _frameGenDynamicCard.Visibility = Shown(frameGen == "dlss");
            _reflexCard.Visibility = Shown(frameGen == "dlss");
            _frameGenCapCard.Visibility = Shown(frameGen != "off");
            SetHint(_frameGenHint, frameGen switch
            {
                "dlss" when !StreamlineFound() => L.T("NVIDIA Streamline (sl.interposer.dll, sl.dlss_g.dll) is missing from the engine folder: the game uses no frame generation"),
                "dlss" when !DlssFrameGenGpu() => L.T("Needs an NVIDIA RTX 40 or 50 GPU with Hardware-accelerated GPU scheduling on; other GPUs: choose FSR 3.1"),
                "dlss" => L.T("RTX 40: 2x; RTX 50: up to 6x. Loaded when the game starts; in game you can switch it off and on (Insert or L3 + R3)"),
                "fsr3" => L.T("Any GPU. Not in menus, movies or with HDR output"),
                _ => null,
            });
            if (frameGen != "off" && value == "off")
                SetHint(_frameGenHint, L.T("Frame generation needs an upscaler (or TAA): choose one above"));
            // NW-666485f: with frame generation the game itself runs near half the shown rate, under what the 90-144
            // patches' timing expects, and lifts drop riders. 60 (or Uncapped with even pacing) keeps them in step.
            else if (frameGen != "off" && S.Str("fps_mode") is "90" or "120" or "144")
                SetHint(_frameGenHint, L.T("With frame generation the game itself runs at about half the frame rate shown, slower than the 90 to 144 FPS patches expect, and lifts can drop you. Choose 60 FPS (or Uncapped with even pacing) for frame generation"));

            // An upscaler takes the finished frame at outputs other than 1920x1080 and can smear the
            // game's motion blur; More > Engine can leave the blur out there (BB_UPSCALER_NO_BLUR).
            var (w, h) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            if (_motionBlurHint != null)
                SetHint(_motionBlurHint, value != "off" && (w, h) != (1920, 1080) && Ini.Flag("effect_motion_blur")
                    ? L.T("With an upscaler at an output other than 1920x1080 the blur can smear. More > Engine > No motion blur smear with upscalers leaves it out there")
                    : null);

            var ngx = Paths.DlssRuntime;
            SetHint(_dlssHint, ngx != null
                ? L.F($"NVIDIA DLSS {Paths.DlssVersion(ngx)} is included ({Path.Combine("out", Path.GetFileName(ngx))}). Choose it as the upscaler above")
                : L.F($"{Path.Combine("out", "nvngx_dlss.dll")} is missing from the engine folder"));
        }

        /// <summary>The fsr4vk provider the package ships (FSR 4.1.1 on any GPU, vk_fsr4vk.cpp).</summary>
        private static bool Fsr4VkFound() =>
            File.Exists(Path.Combine(Paths.EngineDir, "fsr4vk", "amd_fidelityfx_upscaler_vk.dll"));

        private static bool FsrSdkFound() =>
            File.Exists(Path.Combine(Paths.EngineDir, "fsr_sdk", "amd_fidelityfx_loader_vk.dll")) &&
            File.Exists(Path.Combine(Paths.EngineDir, "fsr_sdk", "amd_fidelityfx_upscaler_vk.dll"));

        private static bool Amd() => Hardware.Detect().Vendor == "AMD";
    }
}
