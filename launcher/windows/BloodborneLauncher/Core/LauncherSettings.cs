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
        public const string Discord = "https://discord.gg/sBK9FKKeup";
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
            // Beta builds: on (Load switches older files once), so crash logs carry the frame stats.
            ["frame_stats"] = Updater.CurrentBeta > 0,
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
            ["discord_game_place"] = true,
            ["discord_game_stats"] = true,
            ["safe_mode_next"] = false,
            ["memory_check_off"] = false,
            ["discord_hidden"] = false,
            ["hardware_offered"] = false,
            ["setup_shown"] = false, // the setup guide opened once (Views/SetupGuide.cs)
            ["cheat_vials_amount"] = 20,
            ["cheat_bullets_amount"] = 20,
            ["update_channel"] = "", // empty: beta builds follow beta, others stable
            ["ui_theme"] = "eldritch", // since 0.2.5 (EldritchMarker); "default" is the plain look
            ["ui_theme_colour"] = "#C41E3A",
            ["ui_background"] = "game", // game | custom | none
            ["ui_background_image"] = "",
            ["ui_background_blur"] = "strong", // strong | light | off
            ["app_icon"] = "auto", // auto only (AppIcon)
            ["fsr4_balanced"] = false,
            ["exp_vram_cleanup"] = true,
            // 0.9.37 beta 4: the dev setup, measured faster (PerfDefaults below switches older files once).
            ["exp_frame_pacing"] = true,
            ["exp_thread_priority"] = true,
            ["exp_sleep_spin"] = true,
            ["exp_async_pipelines"] = true,
            ["exp_fast_loading"] = true,
            ["exp_hot_pages"] = true,
            ["exp_guest_in_place"] = true,
            ["exp_native_calls"] = true,
            ["exp_sfx_workaround"] = false,
            ["exp_camera_easing"] = false,
            ["borderless_fullscreen"] = false,
            // The new engine features (EnginePreset New): on since 0.9.34, stable builds too.
            // Beta 9.9: off by default (objects and floors popped in late or flickered).
            ["exp_occlusion"] = false,
            ["exp_fast_draws"] = true,
            ["exp_compile_skip"] = true,
            ["exp_vram_budget"] = true,
            // Still being tested: on in beta builds only.
            ["exp_sharp_ui"] = Updater.CurrentBeta > 0,
            ["audio_priority"] = false,
            ["sound_output"] = "",
            ["mute_unfocused"] = false,
            ["load_profile"] = false,
            ["occlusion_default_on"] = true,
            ["vram_budget_default_on"] = true,
            ["section_stats"] = false,
            ["shader_capture"] = false,
            ["heap_watch_log"] = false,
            ["net_rxgap_log"] = false,
            ["overlay_keys"] = "both",
            ["dlss_preset"] = "k",
            ["legacy_shaders"] = false, // beta 9.8 made legacy the default; beta 4 the native one again (PerfDefaults)
            ["trophy_notifications"] = true,
            ["map_enabled"] = true,  // the in-game map (Display › Map); on by default since 0.2.4 (MapOnMarker)
            ["trophy_sound"] = true,
            ["trophy_volume"] = 35,  // NW-e2243ba: 70 was too loud for the first trophy
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
                    // Beta builds: textures kept up to 80% of the card (the 40% default evicted and
                    // reloaded hundreds a second on 6-8 GB cards); switched on once for older files.
                    // The marker is only ever written by a beta: "vram_budget_default_on" is in
                    // Defaults(), so a stable launcher saved it beside its own default (off) and a
                    // player moving from stable to a beta kept the 40% marks (RTX 4060 at 3.4-4.3
                    // GB of 2486 MiB: evictions, re-uploads and tiled write-backs every frame).
                    if (Updater.CurrentBeta > 0 && !saved.ContainsKey("vram_budget_beta_on"))
                        _data["exp_vram_budget"] = true;
                    // 0.9.37 beta 8: beta builds write the frame statistics into the log, switched
                    // on once. Ebrietas' device losses were told apart only in the one log that had
                    // them (the game's GPU range allocator nearly empty); most reports came without.
                    if (Updater.CurrentBeta > 0 && !saved.ContainsKey("frame_stats_beta_on"))
                        _data["frame_stats"] = true;
                    // Beta 9.9: occlusion culling off once (9.8 switched it on for everyone; floors
                    // and objects appeared late or flickered). On again only by choice.
                    // 0.2.4: the in-game map is on by default, switched on once for older files
                    // (they saved the old default, off). Off again only by choice.
                    if (!saved.ContainsKey(MapOnMarker))
                        _data["map_enabled"] = true;
                    // 0.2.5: the Eldritch theme is the default, switched on once for older files that
                    // kept the old default look; a colour theme the player chose stays.
                    if (!saved.ContainsKey(EldritchMarker) && Str("ui_theme") == "default")
                        _data["ui_theme"] = "eldritch";
                    if (!saved.ContainsKey("occlusion_off_99"))
                        _data["exp_occlusion"] = false;
                    // Beta 9.8: the legacy shader compiler is the default again (the native one
                    // held 45-55 FPS where the legacy one held 60, Forbidden Woods, RTX 3090);
                    // switched on once, the native one stays a choice.
                    if (!saved.ContainsKey("legacy_shaders_default_98"))
                        _data["legacy_shaders"] = true;
                    // 0.9.37 beta 4: the engine options the dev build ran with, once, over older
                    // files (beta 3 players ran with half of the engine's own speedups switched off:
                    // render threads at normal priority, long sleep spins, no precise pacing, every
                    // memory page watched, 1/3 of the CPU and GPU used on an i7-10700F).
                    if (!saved.ContainsKey(PerfDefaultsMarker))
                        foreach (var (key, value) in PerfDefaults)
                            _data[key] = value;
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
            {
                _data["vram_budget_beta_on"] = true;
                _data["frame_stats_beta_on"] = true;
            }
            _data["legacy_shaders_default_98"] = true;
            _data["occlusion_off_99"] = true;
            _data[MapOnMarker] = true;
            _data[EldritchMarker] = true;
            _data[PerfDefaultsMarker] = true;
        }

        private const string PerfDefaultsMarker = "perf_defaults_b4";
        private const string MapOnMarker = "map_on_024";
        private const string EldritchMarker = "theme_eldritch_025";

        /// <summary>0.9.37 beta 4's defaults that older settings files are switched to once.</summary>
        private static readonly (string Key, bool Value)[] PerfDefaults =
        {
            ("exp_frame_pacing", true), ("exp_thread_priority", true), ("exp_sleep_spin", true),
            ("exp_fast_loading", true), ("exp_hot_pages", true), ("exp_guest_in_place", true),
            ("exp_native_calls", true), ("legacy_shaders", false),
        };

        public void Save()
        {
            Directory.CreateDirectory(Paths.ConfigDir);
            // Written beside and moved into place: an interrupted write no longer leaves an empty file
            // that the next start reads as all defaults (remembered passwords gone).
            File.WriteAllText(Paths.ConfigFile + ".tmp",
                _data.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            File.Move(Paths.ConfigFile + ".tmp", Paths.ConfigFile, true);
        }

        /// <summary>The shadNet password, kept encrypted for this Windows user (Core/Secret.cs).</summary>
        public string OnlinePassword
        {
            get => Secret.Unprotect(Str("online_password"));
            // Should Windows refuse to encrypt it, it stays as typed rather than being lost.
            set => Set("online_password", Secret.Protect(value) is { Length: > 0 } encrypted ? encrypted : value);
        }

        public string PartyPassword
        {
            get => Secret.Unprotect(Str("party_password"));
            set => Set("party_password", value.Length == 0 ? "" :
                Secret.Protect(value) is { Length: > 0 } encrypted ? encrypted : value);
        }

        /// <summary>The password last used to join a host, by its code or address as joined (encrypted like
        /// PartyPassword, which stays the hosting password).</summary>
        public string JoinPassword(string host) =>
            Map("party_join_passwords").TryGetValue(host.Trim(), out var p) ? Secret.Unprotect(p) : "";

        public void SetJoinPassword(string host, string value)
        {
            var key = host.Trim();
            if (key.Length == 0) return;
            var map = Map("party_join_passwords");
            if (value.Length == 0) map.Remove(key);
            else map[key] = Secret.Protect(value) is { Length: > 0 } encrypted ? encrypted : value;
            SetMap("party_join_passwords", map);
        }

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
