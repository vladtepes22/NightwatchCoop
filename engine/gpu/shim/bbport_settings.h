// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

namespace BbSettings {
inline constexpr int HdrToneMapperVanilla = 2, HdrToneMapperPsychoScene = 3;
/// hdr_tonemapper = 4 (menu and launcher: PsychoV-30): PsychoV30 on the scene without
/// the match of its shadows to the game's (3, Hybrid: bbport.ini only, no longer in the menu and
/// launcher); otherwise the same (HdrChannel::Tonemapper reports HdrToneMapperPsychoScene).
inline constexpr int HdrToneMapperPsychoPure = 4;
/// hdr_tonemapper = 5 (the default; menu and launcher: Vanilla+): souperman9's Vanilla+ (RenoDX's
/// Bloodborne mod) on the
/// scene, in the same pass (HdrChannel::Tonemapper reports HdrToneMapperPsychoScene too).
inline constexpr int HdrToneMapperVanillaPlus = 5;
/// hdr_gamut (bbport.ini "bt2020" / "bt709"): PsychoV30's target gamut, its
/// gamut_compression_mode (1 BT.2020, RenoDX's HDR default; 0 BT.709).
inline constexpr int HdrGamutBt709 = 0, HdrGamutBt2020 = 1;


enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
inline bool IsFrameUpscaler(int upscaler) {
    return IsFsr4(upscaler) || upscaler == UpscalerDlss;
}
enum FrameGen : int { FrameGenOff = 0, FrameGenFsr3 = 1, FrameGenDlss = 2, FrameGenCount };
inline constexpr const char* FrameGenNames[] = {"off", "fsr3", "dlss"};
static_assert(sizeof(FrameGenNames) / sizeof(FrameGenNames[0]) == FrameGenCount);
inline constexpr int FrameGenFramesMax = 5;
enum Reflex : int { ReflexOff = 0, ReflexOn = 1, ReflexBoost = 2, ReflexCount };
inline constexpr const char* ReflexNames[] = {"off", "on", "boost"};
static_assert(sizeof(ReflexNames) / sizeof(ReflexNames[0]) == ReflexCount);
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCustom, PresetCount };
inline constexpr float RenderScaleMin = 1.0f, RenderScaleMax = 3.0f;
inline constexpr float MipBiasMin = -2.0f, MipBiasMax = 1.0f;
enum FpsPosition : int { FpsTopRight = 0, FpsTopLeft, FpsBottomRight, FpsBottomLeft, FpsTopCentre, FpsPositionCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
enum class MenuLanguage {
    English, Russian, Spanish, PortugueseBrazil, French, German, Italian, Polish, Turkish, ChineseSimplified
};
inline constexpr const char* MenuLanguageCodes[] = {"en", "ru", "es", "pt_BR", "fr", "de", "it", "pl", "tr", "zh_CN"};
inline constexpr int MenuLanguageCount = int(sizeof(MenuLanguageCodes) / sizeof(MenuLanguageCodes[0]));
static_assert(MenuLanguageCount == int(MenuLanguage::ChineseSimplified) + 1);

struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
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
inline constexpr int OutputWidths[] = {1280,1600,1920,2560,3840,2560,3440,3840,3840,5120,5120,7680,0,0};
inline constexpr int OutputHeights[] = {720,900,1080,1440,2160,1080,1440,1600,1080,1440,2160,2160,0,0};
inline constexpr int OutputCount = 14;
inline constexpr int OutputAuto = 12;
inline constexpr int OutputCustom = 13;
bool ValidOutputSize(int w, int h);
inline constexpr int OutputDefault = 2;
static_assert(sizeof(OutputWidths) / sizeof(OutputWidths[0]) == OutputCount &&
              sizeof(OutputHeights) / sizeof(OutputHeights[0]) == OutputCount);
int OutputWidth(int index);
int OutputHeight(int index);
inline constexpr int CheatOff = 0, CheatInfinite = -1, CheatMaxAmount = 99;
inline constexpr int CheatGunRanges[] = {0, 150, 200, 300};
inline constexpr int CheatEchoMultipliers[] = {1, 2, 5, 10};
/// HDR output (hdr_paper_white / hdr_peak in nits, hdr_boost 0..1): defaults and ranges.
inline constexpr float HdrPaperWhiteDefault = 203.0f, HdrPaperWhiteMin = 10.0f, HdrPaperWhiteMax = 500.0f;
inline constexpr float HdrPeakDefault = 1000.0f, HdrPeakMin = 10.0f, HdrPeakMax = 4000.0f;
inline constexpr float HdrBoostDefault = 0.25f;
/// Native mode and PsychoV30: the game's UI (HUD, menus) at hdr_ui_nits, as RenoDX's UI Brightness
/// (48 .. 500 nits, 203 by default), whatever Game Brightness; hdr_ui_color (native mode) of the
/// way from sRGB's colours (0) to a wide-gamut SDR monitor's (1, DCI-P3). hdr_ui_brightness, the
/// earlier setting (times paper white, 0.5 .. 2), is still read and converted.
inline constexpr float HdrUiNitsDefault = 203.0f, HdrUiNitsMin = 48.0f, HdrUiNitsMax = 500.0f;
inline constexpr float HdrUiBrightnessMin = 0.5f, HdrUiBrightnessMax = 2.0f, HdrUiColorDefault = 1.0f;
enum HdrOutput : int { HdrOutputOff = 0, HdrOutputHdr10 = 1, HdrOutputScRgb = 2 };
/// hdr_mode in lantern.ini (docs/hdr.md). Native: the highlights' brightness from the game's own
/// HDR scene (vk_hdr_native.h), expand where a frame has none (menus, loading, movies). Expand:
/// the SDR picture's highlights stretched towards the peak. HDR16 ("psychov30"): the 16-bit chain.
enum HdrMode : int { HdrModeNative = 0, HdrModeExpand = 1, HdrModePsychoV30 = 2, HdrModeCount };
inline constexpr const char* HdrModeNames[] = {"native", "expand", "psychov30"};
static_assert(sizeof(HdrModeNames) / sizeof(HdrModeNames[0]) == HdrModeCount);

inline constexpr int DlssPresetDefault = 0, DlssPresetK = 11, DlssPresetMax = 26;
inline constexpr int DlssPresetAuto = -1, DlssPresetL = 12, DlssPresetM = 13;
int DlssPresetFromText(const char* text);
const char* DlssPresetText(int preset);

enum OptiSubmit : int { OptiSubmitAuto = -1, OptiSubmitOff = 0, OptiSubmitOn = 1 };

enum ButtonPrompts : int { PromptsAuto = 0, PromptsKeyboard = 1, PromptsController = 2, PromptsCount };
inline constexpr const char* ButtonPromptsNames[] = {"auto", "keyboard", "controller"};
static_assert(sizeof(ButtonPromptsNames) / sizeof(ButtonPromptsNames[0]) == PromptsCount);

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
    /// The party HUD board shows each member's round-trip time in ms after the name, green / yellow / red
    /// by quality (ini party_hud_ping, default on). Live, local.
    std::atomic<bool> party_hud_ping{true};
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
    /// 0.7.5: host over a VPN (Radmin, Hamachi, ZeroTier, Tailscale): 0 auto (a VPN adapter that is
    /// up, else the usual address), 1 off, 2 a named adapter (party_vpn_name). ini party_vpn =
    /// auto|off|<adapter name>; env BB_PARTY_VPN overrides. The host's choice; read when the game
    /// starts (the address is chosen once). The name itself lives in bbnet (set_vpn_preference).
    std::atomic<int> party_vpn{0};
    /// 0.7.5: party discovery (net/party_discovery.h): the host announces itself on the VPN and LAN adapters
    /// only; the launcher's Join page lists what it hears. ini party_discovery, env BB_PARTY_DISCOVERY. Live.
    std::atomic<bool> party_discovery{true};
    /// 0.7.6: connection warnings (party/party_connwarn.h): an on-screen notice when the firewall, the router or a
    /// wrong address keeps the party from working. Per player. ini party_conn_warnings. Live.
    std::atomic<bool> party_conn_warnings{true};
    /// 0.7.7: the notice "<item>: shared with the party" on the screen when a shared item (key / unique item, weapon,
    /// rune) is given (party/party_item_notice_rules.h). Per player; shows only while the party notices
    /// (party_toasts) are on. ini party_item_notices. Live.
    std::atomic<bool> party_item_notices{true};
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
    /// 0.7.5: Party starts = skip the prologue (party_start.h StartMode::SkipPrologue): a NEW character
    /// begins as after the prologue. The host's rule; OFF by default.
    std::atomic<bool> party_skip_prologue{false};
    /// 0.7.7 (bosstarget): bosses share their attention between the players in reach instead of keeping one
    /// target (party_enemyai_rules.h PickAttention). Acts on the AI owner's game (the host). On by default.
    std::atomic<bool> party_boss_share{true};
    std::atomic<bool> party_npc_fights{true};   ///< D11 (0.7.3): NPC hunters fight the whole party (party_npcfight_rules.h)
    std::atomic<bool> party_any_kidnap{true};   ///< 0.7.3: a Snatcher grabbing a guest takes the party to the Gaol (party_kidnap_rules.h)
    /// 0.7.5 (levelsync, party_levelsync_rules.h): the host's rules, both ON = Bloodborne's own co-op.
    /// Level sync: a hunter above the host's level is scaled down (max HP, stamina, defence, resistances)
    /// as in password co-op; phantom health: a hunter joining a party has 30 % less max HP.
    std::atomic<bool> party_level_sync{true};
    std::atomic<bool> party_phantom_hp{true};
    /// 0.7.6 (pvphp): the host's rule "Full health in PvP": no phantom health cut while friendly fire is on.
    std::atomic<bool> party_pvp_full_hp{true};
    /// 0.7.6 (bossdrops): the host's rule: a world boss's item drop also goes to the guests present at the kill.
    std::atomic<bool> party_boss_drops{true};
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
    std::atomic<int> frame_gen{FrameGenOff};
    std::atomic<bool> frame_gen_cap{true};
    std::atomic<bool> frame_gen_active{false};
    std::atomic<float> frame_gen_shown_fps{0.0f}, frame_gen_game_fps{0.0f};
    std::atomic<int> frame_gen_multiplier{2};
    std::atomic<int> frame_gen_frames{1};
    std::atomic<bool> frame_gen_dynamic{false};
    std::atomic<int> frame_gen_target_fps{0};
    std::atomic<bool> dlss_fg_dynamic_supported{false};
    std::atomic<int> reflex{ReflexOn};
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
    std::atomic<float> fsr3_velocity_factor{1.0f}, fsr3_reactiveness_scale{1.0f},
        fsr3_shading_change_scale{1.0f}, fsr3_accumulation{0.333f}, fsr3_min_disocclusion{-0.333f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    std::atomic<bool> show_fps_rate{true};
    std::atomic<bool> show_fps_frame_time{true};
    std::atomic<bool> show_fps_upscaler{true};
    std::atomic<bool> show_fps_cpu{false};
    std::atomic<bool> show_fps_gpu{false};
    std::atomic<bool> show_compile_note{true};
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
    /// Travel to a lamp selected on the map view (map_travel; needs direct lamp travel on).
    std::atomic<bool> map_travel{true};
    std::atomic<int> map_npcs{0};
    /// The mouse cursor hidden over the game 2 s after the mouse last moved (window.cpp).
    std::atomic<bool> hide_cursor{true};
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    std::atomic<bool> motion_blur_held{false};
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<bool> camera_controls{false};
    std::atomic<bool> widescreen{true},pc_controls{true},mouse_camera{true},mouse_menu{true};
    std::atomic<bool> menu_move{false};
    std::atomic<bool> mouse_invert_x{false},mouse_invert_y{false},mouse_auto_rotation{false};
    std::atomic<bool> pc_menus{true};
    std::atomic<int> mouse_sensitivity{5};
    std::atomic<int> button_prompts{PromptsAuto};
    std::atomic<int> pad_icons{PadIconsAuto};
    std::atomic<bool> change_appearance{true}, rebirth{true};  // on by default since 0.2.5 (settings_rev 3)
    std::atomic<bool> graphics_controls{false}, graphics_vignette{true};
    std::atomic<float> graphics_ao_strength{1},graphics_shadow_scale{1},graphics_bloom{1},graphics_saturation{1};
    std::atomic<float> graphics_fog{1},graphics_brightness{1},graphics_contrast{1},graphics_hue{0};
    std::atomic<float> camera_fov_scale{1.0f}, camera_distance_scale{1.0f}, camera_height_scale{1.0f};
    std::atomic<int> model_lod{0};
    std::atomic<int> output_res{OutputDefault};
    int custom_output_w = 1920, custom_output_h = 1080;
    std::atomic<int> cheat_vials{CheatOff}, cheat_bullets{CheatOff};
    std::atomic<bool> cheat_stamina{false}, cheat_stamina_idle{false}, cheat_stealth{false}, cheat_keep_echoes{false},
        cheat_no_durability{false}, cheat_consumables{false};
    std::atomic<int> cheat_gun_range{0}, cheat_echo_multiplier{1};
    std::atomic<bool> fullscreen{false};
    std::atomic<bool> hdr{false};
    std::atomic<float> hdr_paper_white{HdrPaperWhiteDefault}, hdr_peak{HdrPeakDefault},
        hdr_boost{HdrBoostDefault};
    std::atomic<bool> hdr_peak_auto{true};
    std::atomic<float> hdr_display_peak{0.0f};
    std::atomic<bool> hdr_compare{false}; ///< Native / Expand: the left half as SDR
    std::atomic<float> hdr_ui_nits{HdrUiNitsDefault}, hdr_ui_color{HdrUiColorDefault};
    /// HdrMode (hdr_mode, BB_HDR_MODE); live. In native mode boost is the share of the scene's
    /// highlight range shown: sqrt(boost) of it in log2 (0.25: half, 1: all of it).
    std::atomic<int> hdr_mode{HdrModeNative};
    std::atomic<int> hdr_output{HdrOutputOff};
    std::atomic<int> live_resolution{0};
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false}, dlss_supported{false};
    std::atomic<int> dlss_preset{DlssPresetK}, dlss_applied_preset{-1};
    std::atomic<bool> dlss_mip_bias{true};
    std::atomic<bool> dlss_auto_exposure{false};
    std::atomic<float> render_scale{1.3f};
    std::atomic<float> mip_bias{0.0f};
    std::atomic<bool> optiscaler_compat{true}, optiscaler_ngx_lock{true}, optiscaler_clean_thread{true};
    std::atomic<int> optiscaler_sync_submit{OptiSubmitAuto};
    struct OptiScalerSwitches {
        bool compat = true, ngx_lock = true, clean_thread = true;
        int sync_submit = OptiSubmitAuto;
    } startup_optiscaler;

    int startup_preset = NativeAA;
    float startup_render_scale = 1.3f;
    int startup_frame_gen = FrameGenOff;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
    std::atomic<float> hdr_exposure{1.0f};
    std::atomic<float> hdr_contrast{50.0f};
    std::atomic<float> hdr_grade_highlights{50.0f};
    std::atomic<float> hdr_shadows{50.0f};
    std::atomic<float> hdr_saturation{50.0f};
    std::atomic<float> hdr_highlight_saturation{50.0f};
    std::atomic<float> hdr_blowout{0.0f};
    std::atomic<float> hdr_bloom{50.0f};    ///< the game's bloom, 0..100, 50 as drawn
    std::atomic<float> hdr_scene_grading{100.0f}; ///< the game's color table, 0..100 % (mode 5)
    std::atomic<float> hdr_vignette{50.0f}; ///< the game's vignettes, 0..100, 50 as drawn
    std::atomic<float> hdr_black_floor{0.0f}; ///< nits of the game's black taken down to 0 (0..2)
    /// PsychoV30's gamut_compression (RenoDX's Gamut Compression): 0 no mapping into the target
    /// gamut .. 1 the full mapping.
    std::atomic<float> hdr_gamut_compression{1.0f};
    /// PsychoV30's compression (RenoDX's Compression): the shoulder's power toward the peak,
    /// 0 automatic, 0.1..2 manual (1 RenoDX's default).
    std::atomic<float> hdr_compression{1.0f};
    /// How the 16-bit image goes through the game's color table (bb_color_grading.frag): 0 Exact
    /// (the game's own grade up to white), 1 RenoDX (its LUT bridge as it is). bbport.ini
    /// hdr_lut_bridge = exact / renodx.
    std::atomic<int> hdr_lut_bridge{0};
    std::atomic<float> hdr_knee{60.0f}; ///< mode 5: % of the game's white where HDR starts
    /// Set by the presenter: the frame is sent in HDR10 (the menu shows the HDR section).
    /// Set by the presenter: the all-areas HDR highlights are on (strength and boost apply).
    std::atomic<bool> hdr_highlights{false};
    // HDR tests (in-game menu, this session only; BB_HDR_DEBUG / BB_HDR_TEST_PATTERN at start).
    enum HdrCompare : int { HdrCompareHdr = 0, HdrCompareSdr = 1, HdrCompareCount };
    std::atomic<int> hdr16_compare{HdrCompareHdr}; ///< HDR16: F9 cycles HDR / the game's SDR
    std::atomic<int> hdr_debug{0};               ///< 1 gain as colors, 2 pixels taken as UI
    std::atomic<bool> hdr_test_pattern{false};
    std::atomic<int> hdr_native16_decode{0}; ///< mode 5 test: 0 auto, 1 linear, 2 sRGB signal
    std::atomic<int> hdr_native16_probe{0};  ///< mode 5 test: chain stage shown in magenta
    std::atomic<bool> hdr_chain_trace{false}; ///< mode 5 test: log one frame of the 16-bit chain
    // Set by the renderer, shown by the menu.
    std::atomic<bool> hdr_channel_live{false};   ///< this frame has HDR highlights
    std::atomic<bool> hdr_native16_live{false}; ///< this frame is the 16-bit chain's
    std::atomic<unsigned> hdr_variant{0};        ///< last composite with the HDR pass
    std::atomic<int> hdr_tonemapper{HdrToneMapperVanillaPlus};
    std::atomic<int> hdr_gamut{HdrGamutBt2020}; ///< PsychoV30's target gamut, live
    /// The in-game menu's HDR preset (PsychoV30), as RenoDX's: 0 Off (the game's own look),
    /// 1..3 the player's own settings, each kept in bbport.ini (hdr_preset1..3); the sliders
    /// change the one selected.
    std::atomic<int> hdr_preset{1};
    /// The in-game menu's Settings Mode (RenoDX's): 0 Simple, 1 Intermediate, 2 Advanced.
    std::atomic<int> hdr_settings_mode{0};
    bool startup_psychov30 = false;

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
constexpr int kSettingsRev = 3;
extern const int PartySwitchCount;
/// party_guest_insight's ini / env values by index (parity, full, 0).
const char* GuestInsightName(int mode);

Values& Get();
inline float HdrPeakNits(const Values& v) {
    const float display = v.hdr_display_peak.load(std::memory_order_relaxed);
    const float peak = v.hdr_peak_auto.load(std::memory_order_relaxed) && display >= HdrPeakMin
                           ? display
                           : v.hdr_peak.load(std::memory_order_relaxed);
    return peak < HdrPeakMin ? HdrPeakMin : peak > HdrPeakMax ? HdrPeakMax : peak;
}
const char* MenuText(const char* english, const char* russian);
inline const char* MenuText(const char* english) { return MenuText(english, english); }

void Load();
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool dlss = false);
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes). With an HDR preset selected, it also keeps the HDR settings
/// as that preset's.
void Save();

inline constexpr int HdrPresetCount = 4; ///< Off and three of the player's
/// Selects an HDR preset: the HDR settings go to the one selected before, and those of the new
/// one (1..3) are loaded; a preset never saved starts from the defaults. 0 (Off) keeps them.
void SelectHdrPreset(int preset);
/// The PsychoV30 HDR settings of the selected preset back to the defaults (the menu's Reset all).
void ResetHdr();
/// The display's peak luminance (hdr_display_peak, 0: unknown; window.cpp sets it, tests too).
void SetDisplayPeak(float nits);
float DisplayPeak();
/// Peak Brightness default: the display's peak, else 1000 nits.
float DefaultPeakNits();
/// Game Brightness default: 203 nits (BT.2408's reference white), for every display.
float DefaultGameNits();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
int ModelPreset(int preset);
bool DlssPresetValid(int preset);
const char* UpscalerName(int upscaler);

}
