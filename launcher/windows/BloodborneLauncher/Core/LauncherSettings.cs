using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// settings.json, shared with the Windows port's own launcher (same folder, same keys).
    /// Unknown keys are kept, so neither launcher loses the other's settings.
    /// </summary>
    public sealed class LauncherSettings
    {
        public const string OnlineCommunity = "https://thehuntersdream.com";
        public const string Discord = "https://discord.gg/d3Uv3Cfr5x";
        public const string ShadnetServer = "srv.shadps4.net:31313";

        private JsonObject _data = new();

        private static Dictionary<string, JsonNode?> Defaults() => new()
        {
            ["ui_language"] = "en",
            ["game_dir"] = "",
            ["user_dir"] = "",
            ["dlc_dir"] = "",
            ["player_profile"] = "",
            ["player_profiles"] = "",
            ["mods_dir"] = "",
            ["mods_enabled"] = true,
            ["patches_dir"] = "",
            ["language"] = "1",
            ["fullscreen"] = false,
            ["display_device"] = "",
            ["present_mode"] = "Mailbox",
            ["fps_mode"] = "60",
            ["draw_pipe"] = "",
            ["readbacks"] = "",
            ["frame_stats"] = false,
            ["gpu_profile"] = false,
            ["vk_validation"] = false,
            ["extra_env"] = "",
            ["dlc_enabled"] = true,
            ["online_password"] = "",
            // This launcher.
            ["port_dir"] = "",
            ["launch_method"] = "windows",
            ["custom_command"] = "",
            ["custom_args"] = "",
            ["switch_to_output"] = true,
            ["check_updates"] = true,
            ["save_backups"] = true,
            ["discord_status"] = "off",
            ["discord_timer"] = true,
            ["safe_mode_next"] = false,
            ["memory_check_off"] = false,
            ["discord_hidden"] = false,
            ["hardware_offered"] = false,
            ["cheat_vials_amount"] = 20,
            ["cheat_bullets_amount"] = 20,
            ["update_channel"] = "", // empty: beta builds follow beta, others stable
            ["ui_layout"] = "modern",
            ["ui_theme"] = "default",
            ["ui_theme_colour"] = "#C41E3A",
            ["ui_background"] = "game", // game | custom | none
            ["ui_background_image"] = "",
            ["ui_background_blur"] = "strong", // strong | light | off
            ["ui_window_buttons"] = "mac", // mac | windows
            ["ui_window_buttons_side"] = "usual", // usual (mac left, windows right) | left | right
            ["fsr4_balanced"] = false,
            ["exp_vram_cleanup"] = true,
            ["exp_frame_pacing"] = false,
            ["exp_thread_priority"] = false,
            ["exp_sleep_spin"] = false,
            ["exp_async_pipelines"] = true,
            ["exp_fast_loading"] = false,
            ["exp_hot_pages"] = false,
            ["exp_sfx_workaround"] = false,
            ["exp_camera_easing"] = false,
            ["borderless_fullscreen"] = false,
            // New engine features that could still crash or hang: on in beta builds, where they are
            // tested, and off in stable ones until they have been.
            ["exp_occlusion"] = Updater.CurrentBeta > 0,
            ["exp_fast_draws"] = Updater.CurrentBeta > 0,
            ["exp_compile_skip"] = Updater.CurrentBeta > 0,
            ["exp_vram_budget"] = Updater.CurrentBeta > 0,
            ["exp_sharp_ui"] = Updater.CurrentBeta > 0,
            ["audio_priority"] = false,
            ["sound_output"] = "",
            ["load_profile"] = false,
            ["occlusion_default_on"] = true,
            ["vram_budget_default_on"] = true,
            ["section_stats"] = false,
            ["shader_capture"] = false,
            ["overlay_keys"] = "both",
            ["dlss_preset"] = "k",
            ["legacy_shaders"] = false,
            ["trophy_notifications"] = true,
            ["trophy_sound"] = true,
            ["trophy_volume"] = 70,
            ["trophy_position"] = "top-right",
        };

        public void Load()
        {
            _data = new JsonObject();
            foreach (var (key, value) in Defaults())
                _data[key] = value;

            try
            {
                var initial = Path.Combine(AppContext.BaseDirectory, "seamless-defaults.json");
                if (!File.Exists(Paths.ConfigFile) && File.Exists(initial) &&
                    JsonNode.Parse(File.ReadAllText(initial)) is JsonObject firstRun)
                    foreach (var (key, value) in firstRun) _data[key] = value?.DeepClone();
                if (File.Exists(Paths.ConfigFile) &&
                    JsonNode.Parse(File.ReadAllText(Paths.ConfigFile)) is JsonObject saved)
                {
                    foreach (var (key, value) in saved)
                        _data[key] = value?.DeepClone();
                    // Beta builds: occlusion culling no longer flickers and is on by default; files
                    // from earlier versions saved the old default (off), so it is switched on once.
                    if (Updater.CurrentBeta > 0 && !saved.ContainsKey("occlusion_default_on"))
                        _data["exp_occlusion"] = true;
                    // Beta builds: textures kept up to 80% of the card (the 40% default evicted and
                    // reloaded hundreds a second on 6-8 GB cards); switched on once for older files.
                    // The marker is only ever written by a beta: "vram_budget_default_on" is in
                    // Defaults(), so a stable launcher saved it beside its own default (off) and a
                    // player moving from stable to a beta kept the 40% marks (RTX 4060 at 3.4-4.3
                    // GB of 2486 MiB: evictions, re-uploads and tiled write-backs every frame).
                    if (Updater.CurrentBeta > 0 && !saved.ContainsKey("vram_budget_beta_on"))
                        _data["exp_vram_budget"] = true;
                    // Older launchers kept the shadNet password in plain text: encrypted now, and
                    // the file rewritten without it.
                    if (Str("online_password") is { Length: > 0 } plain && !Secret.IsProtected(plain))
                    {
                        OnlinePassword = plain;
                        Save();
                    }
                }
            }
            catch
            {
                // A broken file falls back to the defaults; it is rewritten on the next save.
            }
            // Saved with the next change: from then on the player's own choice stays.
            if (Updater.CurrentBeta > 0)
                _data["vram_budget_beta_on"] = true;
        }

        public void Save()
        {
            Directory.CreateDirectory(Paths.ConfigDir);
            File.WriteAllText(Paths.ConfigFile,
                _data.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        }

        /// <summary>The shadNet password, kept encrypted for this Windows user (Core/Secret.cs).</summary>
        public string OnlinePassword
        {
            get => Secret.Unprotect(Str("online_password"));
            // Should Windows refuse to encrypt it, it stays as typed rather than being lost.
            set => Set("online_password", Secret.Protect(value) is { Length: > 0 } encrypted ? encrypted : value);
        }

        /// <summary>The Hunter's Dream account token (its device link), encrypted like the password.</summary>
        public string HuntersDreamToken
        {
            get => Secret.Unprotect(Str("hd_token"));
            set
            {
                Set("hd_token", value.Length == 0 ? "" : Secret.Protect(value) is { Length: > 0 } encrypted ? encrypted : value);
                // A new sign-in (or none): a refusal was for the token before it.
                Set("hd_refused", "");
            }
        }

        public string PartyPassword
        {
            get => Secret.Unprotect(Str("party_password"));
            set => Set("party_password", value.Length == 0 ? "" :
                Secret.Protect(value) is { Length: > 0 } encrypted ? encrypted : value);
        }

        /// <summary>Why The Hunter's Dream refused this PC's sign-in in a game (hd_refused, the server's
        /// reason), until the player signs in again; empty when it did not.</summary>
        public string HuntersDreamRefused => Str("hd_refused");

        public string Str(string key)
        {
            var node = _data[key];
            if (node is JsonValue v)
            {
                if (v.TryGetValue<string>(out var s)) return s;
                return v.ToJsonString().Trim('"');
            }
            return "";
        }

        public bool Bool(string key)
        {
            var node = _data[key];
            if (node is JsonValue v)
            {
                if (v.TryGetValue<bool>(out var b)) return b;
                if (v.TryGetValue<int>(out var i)) return i != 0;
                if (v.TryGetValue<string>(out var s)) return s is "1" or "true" or "True";
            }
            return false;
        }

        public int Int(string key)
        {
            var node = _data[key];
            if (node is JsonValue v)
            {
                if (v.TryGetValue<int>(out var i)) return i;
                if (v.TryGetValue<double>(out var d)) return (int)d;
                if (v.TryGetValue<string>(out var s) && int.TryParse(s, out i)) return i;
            }
            return 0;
        }

        /// <summary>A string-to-string object (e.g. key_bindings), empty when absent.</summary>
        public Dictionary<string, string> Map(string key)
        {
            var map = new Dictionary<string, string>(StringComparer.Ordinal);
            if (_data[key] is JsonObject obj)
                foreach (var (k, v) in obj)
                    if (v is JsonValue value && value.TryGetValue<string>(out var s))
                        map[k] = s;
            return map;
        }

        public void SetMap(string key, IReadOnlyDictionary<string, string> map)
        {
            var obj = new JsonObject();
            foreach (var (k, v) in map.OrderBy(p => p.Key, StringComparer.Ordinal))
                obj[k] = v;
            _data[key] = obj;
        }

        public void Set(string key, string value) => _data[key] = value;

        /// <summary>Forgets a setting: it reads as absent ("" / false / 0) again.</summary>
        public void Remove(string key) => _data.Remove(key);

        /// <summary>Every setting as stored (shared settings).</summary>
        public IEnumerable<KeyValuePair<string, JsonNode?>> All => _data;

        public void SetNode(string key, JsonNode value) => _data[key] = value.DeepClone();
        public void Set(string key, bool value) => _data[key] = value;
        public void Set(string key, int value) => _data[key] = value;
    }
}
