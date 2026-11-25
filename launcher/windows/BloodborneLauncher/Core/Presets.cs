using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>Picture and lighting presets for the Graphics page: each fills in the page's own settings
    /// (bbport.ini), which stay editable. The values before the first preset used are kept, so
    /// "Go back" puts them back. Only cosmetic keys: none counts in the party patch hash.</summary>
    public static class Presets
    {
        /// <summary>A setting a preset writes, with the engine's default (bbport_settings.h, Choices.Effects).</summary>
        public sealed record Knob(string Key, string Default, bool Number);

        public sealed record Preset(string Name, string Description, IReadOnlyDictionary<string, string> Values);

        private static Knob Toggle(string key, bool on) => new(key, on ? "1" : "0", false);
        private static Knob Amount(string key, double value) => new(key, Fmt(value), true);

        public static readonly Knob[] Knobs = BuildKnobs();

        private static Knob[] BuildKnobs()
        {
            var knobs = new List<Knob>
            {
                Toggle("graphics_controls", false),
                Toggle("graphics_vignette", true),
                Amount("graphics_ao_strength", 1),
                Amount("graphics_shadow_scale", 1),
                Amount("graphics_bloom", 1),
                Amount("graphics_saturation", 1),
                Amount("graphics_brightness", 1),
                Amount("graphics_contrast", 1),
                Amount("graphics_hue", 0),
                Amount("graphics_fog", 1),
                Amount("graphics_gi_strength", 6),
                Amount("graphics_ssr_intensity", 1),
                Amount("graphics_volumetric_intensity", 0.2),
                Amount("graphics_point_fog_intensity", 0.2),
            };
            // The game's effects and the optional lighting passes; not skip_intro or the debug switches.
            foreach (var effect in Choices.Effects)
                if (effect.Key.StartsWith("effect_", StringComparison.Ordinal))
                    knobs.Add(Toggle(effect.Key, effect.Default));
            return knobs.ToArray();
        }

        private static readonly Dictionary<string, Knob> ByKey = Knobs.ToDictionary(k => k.Key, StringComparer.Ordinal);

        private static IReadOnlyDictionary<string, string> V(params (string Key, string Value)[] values) =>
            values.ToDictionary(v => v.Key, v => v.Value, StringComparer.Ordinal);

        /// <summary>Our own set. A key a preset leaves out gets its default.</summary>
        public static IReadOnlyList<Preset> All { get; } = new[]
        {
            new Preset(L.T("Original"), L.T("The game as it shipped: picture tuning and optional lighting off"), V()),
            new Preset(L.T("Crisp"), L.T("Less haze and no lens effects, a little more contrast. Light on the GPU"), V(
                ("graphics_controls", "1"), ("graphics_vignette", "0"), ("graphics_fog", "0.6"),
                ("graphics_contrast", "1.04"), ("graphics_saturation", "1.05"),
                ("effect_chromatic_aberration", "0"), ("effect_dof", "0"), ("effect_motion_blur", "0"))),
            new Preset(L.T("Atmospheric"), L.T("Thicker fog, glow around lamps and beams of sunlight. Needs an upscaler on"), V(
                ("graphics_controls", "1"), ("graphics_fog", "1.2"), ("graphics_bloom", "1.2"),
                ("graphics_saturation", "0.9"), ("graphics_contrast", "1.03"),
                ("effect_gtao", "1"), ("effect_contact_shadows", "1"),
                ("effect_volumetric_light", "1"), ("graphics_volumetric_intensity", "0.35"),
                ("effect_point_fog", "1"), ("graphics_point_fog_intensity", "0.35"))),
            new Preset(L.T("Full lighting"), L.T("Every optional lighting pass but the experimental ones. Needs an upscaler on and a strong GPU"), V(
                ("effect_ssr", "1"), ("effect_modern_ssr", "1"), ("effect_gtao", "1"), ("effect_pom_cavity", "1"),
                ("effect_contact_shadows", "1"), ("effect_pcss", "1"), ("effect_gi", "1"),
                ("effect_volumetric_light", "1"), ("effect_point_fog", "1"))),
        };

        private static string PreviousFile => Path.Combine(Path.GetDirectoryName(Paths.IniPath)!, "presets-previous.json");

        public static bool HasPrevious => File.Exists(PreviousFile);

        private static string Fmt(double value) => value.ToString("0.00", CultureInfo.InvariantCulture);

        /// <summary>A value as the ini compares it: numbers to two places, toggles as 1 or 0.</summary>
        private static string Normal(Knob knob, string text)
        {
            if (!knob.Number) return text.Trim() == "1" ? "1" : "0";
            return double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) ? Fmt(d) : knob.Default;
        }

        private static string ValueIn(Preset preset, Knob knob) =>
            Normal(knob, preset.Values.TryGetValue(knob.Key, out var value) ? value : knob.Default);

        private static string Current(Knob knob, IniFile ini) => Normal(knob, ini.Get(knob.Key, knob.Default));

        public static bool IsCurrent(Preset preset, IniFile ini) => Knobs.All(k => ValueIn(preset, k) == Current(k, ini));

        public static void Apply(Preset preset, IniFile ini)
        {
            if (!HasPrevious)
            {
                var values = new JsonObject();
                foreach (var knob in Knobs) values[knob.Key] = ini.Raw(knob.Key);
                var staging = PreviousFile + ".saving";
                File.WriteAllText(staging, new JsonObject { ["values"] = values }.ToJsonString());
                File.Move(staging, PreviousFile, overwrite: true);
            }
            foreach (var knob in Knobs) ini.Set(knob.Key, ValueIn(preset, knob));
        }

        /// <summary>Puts back the values from before the first preset used (a key that was unset is unset again).</summary>
        public static void GoBack(IniFile ini)
        {
            if (!HasPrevious) return;
            JsonObject? values;
            try { values = JsonNode.Parse(File.ReadAllText(PreviousFile))?["values"] as JsonObject; }
            catch (JsonException) { values = null; }
            if (values == null) throw new InvalidDataException(L.T("The saved settings are damaged."));
            foreach (var knob in Knobs)
            {
                var kept = values[knob.Key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;
                ini.Set(knob.Key, kept == null ? null : Normal(knob, kept));
            }
            File.Delete(PreviousFile);
        }
    }
}
