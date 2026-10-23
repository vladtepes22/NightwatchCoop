// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
/// FSR 4, FSR 4.1.1 or DLSS: one frame's inputs to a separate upscaler, which writes the
/// output image (TemporalUpscaler::RecordFsr4); FSR 3.1 and TAA are recorded in place.
inline bool IsFrameUpscaler(int upscaler) {
    return IsFsr4(upscaler) || upscaler == UpscalerDlss;
}
/// bbport: frame_gen in bbport.ini (BB_FRAME_GEN): generated frames between the game's own
/// (vk_frame_generation.h): FSR 3.1 frame interpolation (2x), or NVIDIA DLSS Frame Generation
/// through Streamline (vk_streamline.h; RTX 40/50, loaded at start, frame_gen_frames per real one).
enum FrameGen : int { FrameGenOff = 0, FrameGenFsr3 = 1, FrameGenDlss = 2, FrameGenCount };
inline constexpr const char* FrameGenNames[] = {"off", "fsr3", "dlss"};
static_assert(sizeof(FrameGenNames) / sizeof(FrameGenNames[0]) == FrameGenCount);
/// frame_gen_frames: DLSS generated frames per real one (1 = 2x .. 5 = 6x; RTX 50 for more than 1).
inline constexpr int FrameGenFramesMax = 5;
/// reflex in bbport.ini (BB_REFLEX): NVIDIA Reflex low latency while Streamline is loaded; on (at
/// least) whenever DLSS Frame Generation runs, as NVIDIA requires.
enum Reflex : int { ReflexOff = 0, ReflexOn = 1, ReflexBoost = 2, ReflexCount };
inline constexpr const char* ReflexNames[] = {"off", "on", "boost"};
static_assert(sizeof(ReflexNames) / sizeof(ReflexNames[0]) == ReflexCount);
/// Custom (render_scale in bbport.ini): output / render size of the player's choosing.
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCustom, PresetCount };
inline constexpr float RenderScaleMin = 1.0f, RenderScaleMax = 3.0f;
/// mip_bias in bbport.ini: added to the scene's texture LOD bias while an upscaler runs.
inline constexpr float MipBiasMin = -2.0f, MipBiasMax = 1.0f;
/// bbport: where the FPS counter sits (fps_position).
enum FpsPosition : int { FpsTopRight = 0, FpsTopLeft, FpsBottomRight, FpsBottomLeft, FpsTopCentre, FpsPositionCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
/// The menu's language, in the Language selector's order (bbport_overlay.cpp).
enum class MenuLanguage {
    English, Russian, Spanish, PortugueseBrazil, French, German, Italian, Polish, Turkish, ChineseSimplified
};
/// menu_language in bbport.ini, by MenuLanguage; the launcher's BB_MENU_LANGUAGE uses the same codes.
inline constexpr const char* MenuLanguageCodes[] = {"en", "ru", "es", "pt_BR", "fr", "de", "it", "pl", "tr", "zh_CN"};
inline constexpr int MenuLanguageCount = int(sizeof(MenuLanguageCodes) / sizeof(MenuLanguageCodes[0]));
static_assert(MenuLanguageCount == int(MenuLanguage::ChineseSimplified) + 1);

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    // bbport: chromatic aberration, depth of field, motion blur and the game's own anti-aliasing start
    // off - a clearer picture, and the upscalers anti-alias.
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", false},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", false},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", false},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Собственное сглаживание игры", false},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip startup intros", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets. OutputAuto is the
/// display's own size (output_res = auto): BB_DISPLAY_SIZE, which bb-start reads from Windows at
/// start, else 1920x1080. OutputCustom is any other WxH the player wrote (output_res = 1280x1024, a
/// 5:4 screen). Use OutputWidth/OutputHeight, which know both.
inline constexpr int OutputWidths[] = {1280,1600,1920,2560,3840,2560,3440,3840,3840,5120,5120,7680,0,0};
inline constexpr int OutputHeights[] = {720,900,1080,1440,2160,1080,1440,1600,1080,1440,2160,2160,0,0};
inline constexpr int OutputCount = 14;
inline constexpr int OutputAuto = 12;
inline constexpr int OutputCustom = 13;
/// A custom output size is even, 640x360 to 16384x8640 (the swapchain and the game's half-size buffers).
bool ValidOutputSize(int w, int h);
inline constexpr int OutputDefault = 2; ///< 1920x1080, the game's own size
static_assert(sizeof(OutputWidths) / sizeof(OutputWidths[0]) == OutputCount &&
              sizeof(OutputHeights) / sizeof(OutputHeights[0]) == OutputCount);
int OutputWidth(int index);
int OutputHeight(int index);
/// Live cheats (engine/cheats.cpp): cheat_vials / cheat_bullets in bbport.ini are "off",
/// "infinite" or an amount kept in the inventory, 1..CheatMaxAmount.
inline constexpr int CheatOff = 0, CheatInfinite = -1, CheatMaxAmount = 99;
/// cheat_gun_range: "off" (0) or the firearm bullets' range in percent.
inline constexpr int CheatGunRanges[] = {0, 150, 200, 300};
/// cheat_echo_multiplier: Blood Echoes from kills times this (1: the game's own).
inline constexpr int CheatEchoMultipliers[] = {1, 2, 5, 10};
/// HDR output (hdr_paper_white / hdr_peak in nits, hdr_boost 0..1): defaults and ranges.
inline constexpr float HdrPaperWhiteDefault = 200.0f, HdrPaperWhiteMin = 10.0f, HdrPaperWhiteMax = 500.0f;
inline constexpr float HdrPeakDefault = 1000.0f, HdrPeakMin = 10.0f, HdrPeakMax = 4000.0f;
inline constexpr float HdrBoostDefault = 0.25f;
/// Native mode: the game's UI at hdr_ui_brightness times paper white, and hdr_ui_color of the way
/// from sRGB's colours (0) to a wide-gamut SDR monitor's (1, DCI-P3).
inline constexpr float HdrUiBrightnessMin = 0.5f, HdrUiBrightnessMax = 2.0f, HdrUiColorDefault = 1.0f;
/// The swapchain's HDR signal (Values::hdr_output).
enum HdrOutput : int { HdrOutputOff = 0, HdrOutputHdr10 = 1, HdrOutputScRgb = 2 };
/// hdr_mode in bbport.ini (docs/hdr.md). Native: the highlights' brightness from the game's own
/// HDR scene (vk_hdr_native.h), expand where a frame has none (menus, loading, movies). Expand:
/// the SDR picture's highlights stretched towards the peak.
enum HdrMode : int { HdrModeNative = 0, HdrModeExpand = 1, HdrModeCount };
inline constexpr const char* HdrModeNames[] = {"native", "expand"};
static_assert(sizeof(HdrModeNames) / sizeof(HdrModeNames[0]) == HdrModeCount);

/// dlss_preset in bbport.ini (BB_DLSS_PRESET): DLSS's render preset, "default" (0: the DLL's own
/// for each quality mode) or a letter (1 = A .. 26 = Z). K (11, the transformer model) unless set.
inline constexpr int DlssPresetDefault = 0, DlssPresetK = 11, DlssPresetMax = 26;
/// bbport: "auto": the model per quality mode as NVIDIA's DLSS 4.5 pairs them: K (DLAA, Quality,
/// Balanced), M (Performance), L (Ultra Performance).
inline constexpr int DlssPresetAuto = -1, DlssPresetL = 12, DlssPresetM = 13;
int DlssPresetFromText(const char* text);
/// "default" or the preset's letter (lowercase), as bbport.ini keeps it.
const char* DlssPresetText(int preset);

/// optiscaler_sync_submit in bbport.ini (BB_OPTISCALER_SYNC_SUBMIT): one submit thread only while
/// OptiScaler's frame generation is on (auto), always, or never.
enum OptiSubmit : int { OptiSubmitAuto = -1, OptiSubmitOff = 0, OptiSubmitOn = 1 };

/// button_prompts in bbport.ini: the game's button icons drawn as the bound keys while the keyboard
/// or mouse is in use (auto), always (keyboard), or never (controller). engine/key_prompts.h.
enum ButtonPrompts : int { PromptsAuto = 0, PromptsKeyboard = 1, PromptsController = 2, PromptsCount };
inline constexpr const char* ButtonPromptsNames[] = {"auto", "keyboard", "controller"};
static_assert(sizeof(ButtonPromptsNames) / sizeof(ButtonPromptsNames[0]) == PromptsCount);

/// pad_icons in bbport.ini: the game's controller icons as they are (PlayStation's) or as an Xbox
/// controller's buttons; auto follows the gamepad in use. engine/key_prompts.h.
enum PadIcons : int { PadIconsAuto = 0, PadIconsPlayStation = 1, PadIconsXbox = 2, PadIconsCount };
inline constexpr const char* PadIconsNames[] = {"auto", "playstation", "xbox"};
static_assert(sizeof(PadIconsNames) / sizeof(PadIconsNames[0]) == PadIconsCount);

struct Values {
    /// Party mode: short join / leave / travel notices in a top corner (ini party_toasts). Live.
    std::atomic<bool> party_toasts{true};
    /// Party mode: the party HUD, companions' names / health / whereabouts in the bottom left
    /// corner (party_hud.h; ini party_hud). Never shown solo. Live.
    std::atomic<bool> party_hud{true};
    /// Party mode: guests may use world lamps and Dream headstones too; their use moves the whole
    /// party, host-led (ini party_guest_lamps; env BB_PARTY_GUEST_LAMPS overrides). On restart.
    std::atomic<bool> party_guest_lamps{true};
    /// Party rules (party_balance.h; the host's values are the party's, sent to every guest. Live).
    /// C4: the death debuff "rot": each death stacks one of five penalties (5 stacks), a lamp rest
    /// cleanses it (ini party_rot; env BB_PARTY_ROT=0/1 overrides at start). Off by default (user
    /// decision 2026-10-22; on 0.6.0 - 0.7.1): only an unset key is off, a saved value stays.
    std::atomic<bool> party_rot{false};
    /// bbport.ini settings_rev: the one-time changes of saved settings done so far (kSettingsRev),
    /// shared with the launcher (launcher.py migrate_ini_rev; whichever starts first does them).
    std::atomic<int> settings_rev{0};
    /// D7: enemy scaling per extra hunter (coop::balance::Rules): HP / poise in % of Bloodborne's
    /// boss step (100 = vanilla), damage + % (ini party_boss_hp ... party_enemy_poise).
    std::atomic<int> party_boss_hp{150}, party_boss_damage{10}, party_boss_poise{100};
    std::atomic<int> party_enemy_hp{50}, party_enemy_damage{0}, party_enemy_poise{0};
    /// E2: what is shown above party members (coop::balance::Overhead: 0 off, 1 name, 2 name +
    /// level, 3 name + health bar, 4 name + deaths; ini party_overhead). Live, local.
    std::atomic<int> party_overhead{3};
    /// Party name tags: on (default) = in a party our name plate replaces the game's phantom plate
    /// (which hides behind any occluder); off = the game's own plate as in vanilla, ours not drawn
    /// (ini party_name_tags; env BB_PARTY_HUD_GAME_PLATE=1 keeps the game's plate for tests). Live.
    std::atomic<bool> party_name_tags{true};
    /// Companions drawn as plain hunters: on (default) = the other players' characters on this
    /// screen without the game's blue, glowing phantom look; off = that look as in vanilla
    /// (ini party_glow_off; env BB_PARTY_GLOW_OFF=0/1 overrides). Display only: nothing about the
    /// game's cooperator state changes. Live, local.
    std::atomic<bool> party_glow_off{true};
    /// D8: a party member's NPC conversation (subtitles, voice, choices, gifts) is shared with the
    /// members near it (ini party_npc_talk; env BB_PARTY_NPC_TALK overrides). Live, local.
    std::atomic<bool> party_npc_talk{true};
    /// I2: pings - hold the touchpad and press R3 (or V) to mark the spot looked at for the party;
    /// everyone sees it through walls with its distance for 10 s (ini party_pings; env
    /// BB_PARTY_PINGS overrides). Live, local: off = no combo, no sending, others' pings not drawn.
    std::atomic<bool> party_pings{true};
    /// I2: every party member on this map drawn through walls as a small marker with its distance
    /// in its colour (ini party_through_walls; off by default: busy next to the name plates). Live.
    std::atomic<bool> party_through_walls{false};
    /// The marker on this player's own lost-echoes bloodstain in the host's world ("Blood Echoes N"
    /// with the distance, through walls; party_ping StainMark). Off by default (user decision
    /// 2026-10-20); the bloodstain and its echoes are there either way (ini party_stain_marker; env
    /// BB_PARTY_STAIN_MARKER overrides). Live, local.
    std::atomic<bool> party_stain_marker{false};
    /// I1: friendly fire / PvP - party members can hurt each other (party_pvp.h). A party rule: the
    /// host's value is sent to every guest, a guest's own value never counts. Off by default (ini
    /// party_friendly_fire; env BB_PARTY_FRIENDLY_FIRE=0/1 overrides at start). Live.
    std::atomic<bool> party_friendly_fire{false};
    /// Statsaudit finding 5: lamps are shared by the whole party (a lamp any member lit is lit for
    /// everyone, in each member's own save too). A party rule: the host's value counts. On by
    /// default (ini party_share_lamps; env BB_PARTY_SHARE_LAMPS=0/1 overrides at start).
    std::atomic<bool> party_share_lamps{true};
    /// 0.7.2 (user decision 2026-10-21: "host state saves lamps, anybody can light them"): a guest's
    /// lamps from its own world light up in the host's world when it joins (needs Shared lamps). A
    /// party rule: the host's value counts. Off by default (ini party_guests_bring_lamps; env
    /// BB_PARTY_GUESTS_BRING_LAMPS=0/1 overrides at start).
    std::atomic<bool> party_guests_bring_lamps{false};
    /// ... within this many metres of the NPC (0 = anywhere on the same map; ini
    /// party_npc_talk_range; env BB_PARTY_NPC_TALK_RANGE overrides). Live, local.
    std::atomic<int> party_npc_talk_range{25};
    /// F2: the automatic hole-punching rendezvous over the public BitTorrent DHT (party_rdv.h):
    /// OPT-IN, off by default (it posts the guest's IP on the DHT; user decision 2026-10-14). Both
    /// host and guest need it on. ini party_rendezvous; env BB_PARTY_RENDEZVOUS (the launcher's
    /// Party tab) overrides at start. Read when the party starts (System > Party members).
    std::atomic<bool> party_rendezvous{false};
    /// F2 (0.7.1, network_robustness.md 4a): relay for a party member that cannot reach the host
    /// (and use such a member when we cannot): party members only, capped. On by default. ini
    /// party_member_relay; env BB_PARTY_MEMBER_RELAY overrides at start. Serving follows it live.
    std::atomic<bool> party_member_relay{true};
    /// F2 (0.7.1): IPv6 for the party link and the game's UDP when both PCs have a global IPv6
    /// address (tried before any relay). OFF by default (user decision 0.7.1: the party code would
    /// carry the PC's IPv6 address). ini party_ipv6; env BB_PARTY_IPV6 overrides at start. Read when
    /// the party starts.
    std::atomic<bool> party_ipv6{false};
    /// Party gameplay switches (the launcher's Party Rules / My Party tabs, the game's party pages in
    /// System; PartySwitches below): ini key = the member's name, env BB_PARTY_* = 0/1 wins at start.
    /// On by default. Load puts each value into the process environment when its variable is unset,
    /// so the party modules that read their switch once at start see the ini's value (the rows of
    /// those say "next start"); the revive switches are read live from here (party_b2).
    /// The host's own (its death; shown read-only to guests through party_balance's rules):
    std::atomic<bool> party_host_revive{true};       ///< the host gets up at its lamp without a load
    std::atomic<bool> party_host_limbo_revive{true}; ///< the host gets up in the arena after a boss kill
    /// 0.7.2, what else cleanses rot (party_rot_rules.h; only while rot is on; read live):
    std::atomic<bool> party_rot_cleanse_boss{true};   ///< a boss's death: all of it, for the whole party
    std::atomic<bool> party_rot_cleanse_killer{true}; ///< your killer's death: that death's stack
    std::atomic<bool> party_rot_cleanse_echoes{true}; ///< your bloodstain picked up: its death's stack
    /// Each player's own (applies while it is a guest, or to its own world):
    std::atomic<bool> party_guest_revive_inplace{true}; ///< a guest gets up at the lamp without a load
    std::atomic<bool> party_guest_respawn{true};  ///< a dead guest comes back at the host's lamp
    std::atomic<bool> party_guest_refill{true};   ///< vials / bullets refilled with the host's
    std::atomic<bool> party_chalice_rejoin{true}; ///< a guest stays with / comes back to the host's Chalice Dungeon
    std::atomic<bool> party_guest_mark{true};     ///< guests can use the Hunter's Mark
    std::atomic<bool> party_full_rewards{true};   ///< full echoes and drops for guests
    std::atomic<bool> party_items{true};          ///< the host's item rewards given to guests
    std::atomic<bool> party_grant_bells{true};    ///< the bells after the prologue
    std::atomic<bool> party_open_world{true};     ///< no co-op area walls
    std::atomic<bool> party_progress{true};       ///< story progress copied into guests' saves
    std::atomic<bool> party_progress_npc{true};   ///< ... with NPC quest choices
    std::atomic<bool> party_story{true};          ///< cutscenes, time of day, endings shared
    std::atomic<bool> party_story_mirror{true};   ///< ... played live with the host
    /// 0.7.1 (user rule: every party gameplay switch in the menu, the launcher and the ini). Not put
    /// into the environment (PartySwitch::env_export false): read live from here; an explicit env
    /// variable still wins at start. The host's (party_balance Rules; a guest follows the host's):
    std::atomic<bool> party_kill_record{true};  ///< enemies stay dead for the party (party_kill_record.h)
    std::atomic<bool> party_npc_mirror{true};   ///< an NPC gone from the host's world is gone on guests
    std::atomic<bool> party_gather{true};       ///< I5: the host may gather the party (party_gather.h)
    /// 0.7.3 (user decision 2026-10-22): covenant outcomes (Annalise, Alfred, Valtr) stay in each
    /// player's own save (party_progress Category::covenant_outcome). The host's; on by default.
    std::atomic<bool> party_covenant_per_player{true};
    /// 0.7.3: a party travel by lamp, headstone or Hunter's Mark runs only once every member voted
    /// YES (party_vote.h). The host's rule; OFF by default.
    std::atomic<bool> party_travel_vote{false};
    std::atomic<bool> party_npc_fights{true};   ///< D11 (0.7.3): NPC hunters fight the whole party (party_npcfight_rules.h)
    std::atomic<bool> party_any_kidnap{true};   ///< 0.7.3: a Snatcher grabbing a guest takes the party to the Gaol (party_kidnap_rules.h)
    /// Each player's own (while it is a guest; party_items):
    std::atomic<bool> party_key_view{true};     ///< the own key items while summoned (next start)
    std::atomic<bool> party_key_items_summoned{true}; ///< a host-world key item given at once (needs key_view)
    /// Insight a guest gets for a boss the host kills (party_phantom InsightMode): 0 same as the
    /// host (parity), 1 full bonus, 2 the game's own +1 (ini party_guest_insight parity|full|0; env
    /// BB_PARTY_GUEST_INSIGHT). Next start.
    std::atomic<int> party_guest_insight{0};

    std::atomic<MenuLanguage> menu_language{MenuLanguage::English};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<int> frame_gen{FrameGenOff}; ///< FrameGen; live
    /// While generated frames are shown, the game runs at half the refresh rate so every
    /// generated frame reaches the display (any present mode); live.
    std::atomic<bool> frame_gen_cap{true};
    /// Set by the renderer: frame generation is running (the FPS counter shows both rates), and
    /// the frames shown and the game's own frames a second.
    std::atomic<bool> frame_gen_active{false};
    std::atomic<float> frame_gen_shown_fps{0.0f}, frame_gen_game_fps{0.0f};
    /// bbport: frames shown per game frame while frame_gen_active (2 for FSR 3.1, generated + 1
    /// for DLSS): the flip driver's frame limit and cap divide by it. Set by the renderer.
    std::atomic<int> frame_gen_multiplier{2};
    /// DLSS Frame Generation: generated frames per real one (1..FrameGenFramesMax); live.
    std::atomic<int> frame_gen_frames{1};
    /// bbport: DLSS dynamic multi frame generation (frame_gen_dynamic; live; RTX 50, Streamline's
    /// DLSSGMode::eDynamic): the driver picks 2x..(frame_gen_frames + 1)x per moment to reach
    /// frame_gen_target_fps (0: the display's refresh rate), as OptiScaler's ForceDMFG.
    std::atomic<bool> frame_gen_dynamic{false};
    std::atomic<int> frame_gen_target_fps{0};
    /// Set by the renderer: this GPU and driver can run DLSS dynamic multi frame generation.
    std::atomic<bool> dlss_fg_dynamic_supported{false};
    /// Reflex (Reflex); live while Streamline is loaded.
    std::atomic<int> reflex{ReflexOn};
    /// Set by the renderer: Streamline is loaded (frame_gen=dlss at start), DLSS Frame Generation
    /// runs on this GPU, the most frames it may generate per real one, and why not (or null).
    std::atomic<bool> streamline_loaded{false}, dlss_fg_supported{false};
    std::atomic<int> dlss_fg_max_frames{0};
    std::atomic<const char*> dlss_fg_problem{nullptr};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    /// bbport: FSR 3.1's tuning constants (fsr3_* in bbport.ini; live), as OptiScaler offers them:
    /// velocity factor (lower: steadier bright pixels in motion), reactiveness and shading change
    /// scales, accumulation added per frame (lower: less ghosting, more thin-detail flicker) and
    /// the least accumulation at disocclusions (higher: less white-pixel flicker). SDK defaults.
    std::atomic<float> fsr3_velocity_factor{1.0f}, fsr3_reactiveness_scale{1.0f},
        fsr3_shading_change_scale{1.0f}, fsr3_accumulation{0.333f}, fsr3_min_disocclusion{-0.333f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// What the FPS counter shows: the frame rate, the frame time, the upscaler.
    std::atomic<bool> show_fps_rate{true};
    std::atomic<bool> show_fps_frame_time{true};
    std::atomic<bool> show_fps_upscaler{true};
    std::atomic<bool> show_fps_cpu{false}; ///< bbport: the PC's CPU use (bbport_usage.h)
    std::atomic<bool> show_fps_gpu{false}; ///< bbport: the GPU's 3D engine use
    /// "Building shaders" in a corner while pipelines are built in play (compile_note).
    std::atomic<bool> show_compile_note{true};
    /// bbport: the FPS counter's look (fps_* in bbport.ini; live): its corner (FpsPosition),
    /// size, text and background colours (0xRRGGBB), background opacity and border.
    std::atomic<int> fps_position{0};
    std::atomic<float> fps_scale{1.0f};
    std::atomic<unsigned> fps_text_color{0xFFFFFFu};
    std::atomic<unsigned> fps_bg_color{0x0F0F0Fu};
    std::atomic<float> fps_bg_opacity{0.5f};
    std::atomic<float> fps_text_opacity{1.0f};
    std::atomic<bool> fps_border{true};
    /// The map's extras (map_* in bbport.ini; live; map/map_page.cpp), off as in Hunter's Atlas: the
    /// pickups still lying there and which kinds, areas hidden until their boss is dead, and the NPC
    /// conversation sites (MapNpcs: all, new dialogue only, none).
    std::atomic<bool> map_pickups{false};
    std::atomic<bool> map_pickups_healing{true}, map_pickups_insight{true}, map_pickups_equipment{true},
        map_pickups_coldblood{true};
    std::atomic<bool> map_boss_reveal{false};
    std::atomic<int> map_npcs{0};
    /// The mouse cursor hidden over the game 2 s after the mouse last moved (window.cpp).
    std::atomic<bool> hide_cursor{true};
    /// The settings menu's position (fraction of the screen), -1 until it is moved.
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Set by the renderer: the game's motion blur is held off (engine/graphics.cpp), as a temporal
    /// upscaler takes the game's finished frame, blur and all (BB_UPSCALER_NO_BLUR).
    std::atomic<bool> motion_blur_held{false};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<bool> camera_controls{false}; // startup engine hook, opt-in
    std::atomic<bool> widescreen{true},pc_controls{true},mouse_camera{true},mouse_menu{true};
    /// bbport: menu_move, the movement and look keys (and the mouse camera, with mouse_menu off)
    /// stay the character's while a menu is open in the world; the arrow keys navigate it.
    std::atomic<bool> menu_move{false};
    std::atomic<bool> mouse_invert_x{false},mouse_invert_y{false},mouse_auto_rotation{false};
    std::atomic<int> mouse_sensitivity{5};
    std::atomic<int> button_prompts{PromptsAuto}; ///< ButtonPrompts; live
    std::atomic<int> pad_icons{PadIconsAuto}; ///< PadIcons; live
    std::atomic<bool> change_appearance{false}, rebirth{false}; // next launch, engine features
    std::atomic<bool> graphics_controls{false}, graphics_vignette{true};
    std::atomic<float> graphics_ao_strength{1},graphics_shadow_scale{1},graphics_bloom{1},graphics_saturation{1};
    /// Fog opacity (0..1 of the area's), the tone map's brightness (ColorV) and contrast (Contrust)
    /// scales and a hue offset in degrees (ColorH); 1, 1, 1 and 0 leave the area's own.
    std::atomic<float> graphics_fog{1},graphics_brightness{1},graphics_contrast{1},graphics_hue{0};
    std::atomic<float> camera_fov_scale{1.0f}, camera_distance_scale{1.0f}, camera_height_scale{1.0f};
    std::atomic<int> model_lod{0}; ///< -2 highest (opt-in), 0 the game's, 1 lower (2 is read as 1)
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// output_res = OutputCustom's size, from bbport.ini (fixed for the session).
    int custom_output_w = 1920, custom_output_h = 1080;
    /// CheatOff, CheatInfinite or the amount to keep (1..CheatMaxAmount); live.
    std::atomic<int> cheat_vials{CheatOff}, cheat_bullets{CheatOff};
    /// The other cheats (0/1 in bbport.ini); live.
    std::atomic<bool> cheat_stamina{false}, cheat_stealth{false}, cheat_keep_echoes{false},
        cheat_no_durability{false}, cheat_consumables{false};
    /// One of CheatGunRanges / CheatEchoMultipliers; live.
    std::atomic<int> cheat_gun_range{0}, cheat_echo_multiplier{1};
    /// Borderless fullscreen window at the desktop size (F11 toggles; BB_FULLSCREEN overrides).
    std::atomic<bool> fullscreen{false};
    /// HDR output (hdr, BB_HDR): the game's SDR picture shown in an HDR swapchain when the
    /// display offers one (Windows HDR on). Applied at start (startup_hdr).
    std::atomic<bool> hdr{false};
    /// Nits of the SDR picture's white, the display's peak, and the highlight expansion (0: SDR
    /// white stays at paper white, 1: up to the peak); live (BB_HDR_PAPER_WHITE, BB_HDR_PEAK,
    /// BB_HDR_BOOST).
    std::atomic<float> hdr_paper_white{HdrPaperWhiteDefault}, hdr_peak{HdrPeakDefault},
        hdr_boost{HdrBoostDefault};
    /// hdr_peak_auto: the peak the display reports (hdr_display_peak) instead of hdr_peak, when it
    /// reports one; live. BB_HDR_PEAK turns it off.
    std::atomic<bool> hdr_peak_auto{true};
    /// Set by the window: the peak brightness Windows reports for the game's display (DXGI
    /// MaxLuminance, the Windows HDR Calibration app's value when it has run), 0 when unknown.
    std::atomic<float> hdr_display_peak{0.0f};
    /// The in-game menu's SDR | HDR comparison: the left half as SDR. Not saved.
    std::atomic<bool> hdr_compare{false};
    std::atomic<float> hdr_ui_brightness{1.0f}, hdr_ui_color{HdrUiColorDefault};
    /// HdrMode (hdr_mode, BB_HDR_MODE); live. In native mode boost is the share of the scene's
    /// highlight range shown: sqrt(boost) of it in log2 (0.25: half, 1: all of it).
    std::atomic<int> hdr_mode{HdrModeNative};
    /// Set by the renderer: the HDR output in use (HdrOutput).
    std::atomic<int> hdr_output{HdrOutputOff};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false}, dlss_supported{false};
    /// DLSS's render preset (DlssPreset*); live: its feature is created again (vk_dlss.cpp). Set by
    /// DLSS: the preset its feature runs (0 the DLL's default, a refused one included), -1 before.
    std::atomic<int> dlss_preset{DlssPresetK}, dlss_applied_preset{-1};
    /// dlss_mip_bias (BB_DLSS_MIP_BIAS): one more mip level of texture detail under DLSS; live.
    std::atomic<bool> dlss_mip_bias{true};
    /// dlss_auto_exposure: DLSS's own exposure for any input (HDR inputs always use it); live.
    std::atomic<bool> dlss_auto_exposure{false};
    /// render_scale (BB_RENDER_SCALE): the Custom preset's output / render ratio; live as presets.
    std::atomic<float> render_scale{1.3f};
    /// mip_bias (BB_MIP_BIAS): texture LOD bias added while an upscaler runs (MipBiasMin..Max); live.
    std::atomic<float> mip_bias{0.0f};
    /// OptiScaler compatibility (vk_optiscaler.h), optiscaler_* in bbport.ini: the mode, NGX calls
    /// under the queue lock, the Vulkan instance and device made on a thread of their own, one
    /// submit thread (OptiSubmit). The menu edits these; the engine reads startup_optiscaler, and
    /// BB_OPTISCALER_* (the launcher's switch) override that.
    std::atomic<bool> optiscaler_compat{true}, optiscaler_ngx_lock{true}, optiscaler_clean_thread{true};
    std::atomic<int> optiscaler_sync_submit{OptiSubmitAuto};
    struct OptiScalerSwitches {
        bool compat = true, ngx_lock = true, clean_thread = true;
        int sync_submit = OptiSubmitAuto;
    } startup_optiscaler;

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    float startup_render_scale = 1.3f;
    /// frame_gen at start: DLSS Frame Generation loads Streamline before the Vulkan instance.
    int startup_frame_gen = FrameGenOff;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
    bool startup_hdr = false;
};

/// The party gameplay switches (Values::party_host_revive ... party_story_mirror): ini key, the
/// environment variable, the member.
struct PartySwitch {
    const char* key;
    const char* env;
    std::atomic<bool> Values::*member;
    /// Load puts the ini's value into `env` when it is unset (for modules reading it at start);
    /// false: the module reads the member, only an explicit variable is taken (never exported).
    bool env_export = true;
};
extern const PartySwitch PartySwitches[];
/// bbport.ini settings_rev (the launcher's INI_SETTINGS_REV is the same number):
///   1 (0.7.2): party_rot = 0 once. Rot became off by default, and 0.6.0 - 0.7.1 games wrote their
///              default party_rot=1 at every settings save, chosen or not (coordinator decision
///              2026-10-22: turn it off once; a later explicit 1 stays).
///   2 (0.2.1 beta 2): model_lod -2 -> 0 once. Highest came back as an opt-in; files saved before
///              still hold the old -2 that 0.2.1 beta 1 read as 0, so it is not switched on silently.
constexpr int kSettingsRev = 2;
extern const int PartySwitchCount;
/// party_guest_insight's ini / env values by index (parity, full, 0).
const char* GuestInsightName(int mode);

Values& Get();
/// The peak brightness HDR output uses, in nits: the display's (hdr_peak_auto) or hdr_peak.
inline float HdrPeakNits(const Values& v) {
    const float display = v.hdr_display_peak.load(std::memory_order_relaxed);
    const float peak = v.hdr_peak_auto.load(std::memory_order_relaxed) && display >= HdrPeakMin
                           ? display
                           : v.hdr_peak.load(std::memory_order_relaxed);
    return peak < HdrPeakMin ? HdrPeakMin : peak > HdrPeakMax ? HdrPeakMax : peak;
}
/// Localized overlay text; the Language selector itself stays in English. The other languages
/// look the English text up (bbport_menu_i18n.cpp) and show it when they have no translation.
const char* MenuText(const char* english, const char* russian);
/// Text without a Russian translation at its call site: English in Russian too.
inline const char* MenuText(const char* english) { return MenuText(english, english); }

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 or DLSS uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool dlss = false);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ..., Custom: render_scale).
float PresetScale(int preset);
const char* PresetName(int preset);
/// The fixed preset nearest a preset's scale (Custom: its render_scale): the FSR 4 model and
/// DLSS quality mode it runs with.
int ModelPreset(int preset);
/// dlss_preset letters NVIDIA's DLSS 310.9 knows: E, F (CNN), J, K, L, M (transformer); A-D are
/// removed and G-I, N, O fall back to its default.
bool DlssPresetValid(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
