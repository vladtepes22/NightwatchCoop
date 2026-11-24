Nightwatch's own environment switches; visy's are documented in `engine/docs/party/DEBUG_SWITCHES.md`.
The launcher sets most of these, so players normally never need to set them by hand.

## Ray tracing

| Name | What it does | Values and default | Where read |
|---|---|---|---|
| `BB_RT_BOUNCE_MOTION` | Uses object motion vectors for ray traced bounce history. | 0/1; default off | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_ssgi.cpp:128` |
| `BB_RT_CAMERA_MAP` | Enables the ray tracing camera map. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:47` |
| `BB_RT_CUTOUT` | Allows sun rays through animated cut-outs. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:54` |
| `BB_RT_CUTOUT_LAMPS` | Allows lamp rays through animated cut-outs. | 0/1; default off | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:55` |
| `BB_RT_EVICT_HOLD` | Holds ray tracing entries from eviction above a memory threshold. | numeric; default (unclear) | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:58` |
| `BB_RT_FREE_OFF_THREAD` | Frees released scene data on another thread. | 0/1; default off | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:57` |
| `BB_RT_KEEP_PREVIOUS` | Keeps previous ray tracing mesh data. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:52` |
| `BB_RT_LAMPS_EXACT` | (unclear) | (unclear); default (unclear) | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:59` |
| `BB_RT_LAMP_REUSE` | (unclear) | (unclear); default (unclear) | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:60` |
| `BB_RT_NEW_BUDGET` | Limits new ray tracing entries per frame. | integer; default (unclear) | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:228` |
| `BB_RT_PERSISTENT_SCENE` | Keeps still scene meshes in a persistent scene. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:51` |
| `BB_RT_POOL` | Allocates ray tracing geometry in pool chunks. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:49` |
| `BB_RT_RIGID` | Enables rigid mesh motion tracking. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:53` |
| `BB_RT_SCENE_MB` | Sets the ray tracing scene memory size. | integer MB; default (unclear) | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:227` |
| `BB_RT_SETUP_WORKER` | Runs ray tracing mesh setup on a worker. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:56` |
| `BB_RT_SHADOW_RAYS` | Sets the ray tracing shadow ray count. | integer; default (unclear) | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rt_shadow.cpp:230` |

## Renderer and upscaler

| Name | What it does | Values and default | Where read |
|---|---|---|---|
| `BB_DLSS_CLEAN_COLOR` | Enables clean color output handling for DLSS. | 0/1; default off | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp:1507` |
| `BB_DLSS_PRESET_RULES` | Applies the DLSS preset rules table. | 0 disables; otherwise enabled; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_dlss.cpp:192` |
| `BB_FOG_SCALE_COPY` | Controls copying scaled fog into the game's table. | 0/1; default on | `engine/gpu/shim/engine/graphics.cpp:251` |
| `BB_JITTER_SPRITES` | (unclear) | (unclear); default (unclear) | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2275` |
| `BB_MOTION_INCUMBENCY` | Selects which draws retain motion history. | 0 selects old first-come behavior; default new behavior | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_object_motion.cpp:163` |
| `BB_OCCLUSION_BATCH` | Reads occlusion query results once per batch. | 0/1; default off | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_occlusion.cpp:167` |
| `BB_POINT_FOG_STEADY` | Uses steady lamp fog behavior. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_volumetric_light.cpp:557` |
| `BB_SUBMIT_FAIL_STOP` | Stops on submit failures. | 0 disables except device loss; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp:47` |
| `BB_UI_SMOOTH` | Selects UI texture sampling mode. | unset/other = smooth; 0 = game sampling; 2 = additive draws too; default smooth | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4818` |
| `BB_UI_SMOOTH_LOG` | Logs UI smoothing details. | path/value (unclear); default off | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4820` |
| `BB_UPSCALER_HISTORY_CUT` | Resets upscaler history on a camera cut. | 0 disables; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp:64` |
| `BB_UPSCALER_SKIPS` | Enables upscaler handling of skipped frames. | 0/1; default off | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp:71` |
| `BB_VOLUMETRIC_STEADY` | Uses steady sunlight color behavior. | 0/1; default on | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_volumetric_light.cpp:372` |

## Memory

| Name | What it does | Values and default | Where read |
|---|---|---|---|
| `BB_GAME_HEAPS` | Controls enlarged game heaps. | 0 disables; 1 forces; unset automatic; default automatic | `engine/start/patches.cpp:478` |
| `BB_GC_DRIVER_LEVEL` | Changes the texture cache's driver memory accounting policy. | 0 disables; default enabled | `engine/gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1972` |
| `BB_GUEST_PRELOAD` | Controls guest memory preloading. | 0 disables; default (unclear) | `engine/gpu/shim/bbport_guest_memory_win.cpp:33` |
| `BB_SUN_SHADOW_HEAP` | Controls the heap size for the sun shadow atlas. | 0 uses game's atlas size; default enlarged | `engine/start/patches.cpp:1215` |

## Co-op

| Name | What it does | Values and default | Where read |
|---|---|---|---|
| `BB_ENEMY_SWEEP` | Runs an enemy trace sweep using distance, angle and duration lists. | `dists=...;angles=...;secs=...` with optional orbit/start; default off | `engine/gpu/shim/party/party_enemy_trace.inc:67` |
| `BB_ENEMY_TRACE` | Selects an entity for per-frame CSV tracing. | entity; default off | `engine/gpu/shim/party/party_enemy_trace.inc:53` |
| `BB_ENEMY_TRACE_FILE` | Chooses the enemy trace CSV path. | path; default (unclear) | `engine/gpu/shim/party/party_enemy_trace.inc:54` |
| `BB_PARTY_PICKUP_REGION` | 0 keeps the own region's area-boss pickup rule. | 0 disables override; default on | `engine/gpu/shim/party/party_b2.cpp:11290` |
| `BB_PARTY_TEST_OWN_FLAGS` | Sets listed event flags in a guest's own world once it is up. | comma-separated IDs; default unset | `engine/gpu/shim/party/party_b2.cpp:11323` |

## Mods

| Name | What it does | Values and default | Where read |
|---|---|---|---|
| `BB_MOD_MODELS` | Holds the count of enabled mods that replace models or textures. | integer count; default 0 | `engine/start/patches.cpp:1063` |
| `BB_MOD_MODEL_HEAPS` | Controls enlarged graphics heaps for model mods. | 0 disables; otherwise enabled when model count is positive; default enabled | `engine/start/patches.cpp:1062` |

## Launcher/start

| Name | What it does | Values and default | Where read |
|---|---|---|---|
| `BB_CONFIG` | Selects the settings INI path. | path; default launcher-provided | `engine/gpu/shim/bbport_settings.cpp:125` |
| `BB_OUT_DIR` | Selects the output directory used by party runtime files. | path; default derived by code | `engine/gpu/shim/party/party_runtime.cpp:282` |
| `BB_REBIRTH_ASSET` | Selects a rebirth asset. | path/value; default unset | `engine/gpu/shim/engine/rebirth.cpp:134` |
| `BB_SETTINGS_SCREEN` | Chooses the port Settings screen; 0 uses the game's System list. | 0/other; default on | `engine/gpu/shim/bbport_settings_screen.cpp:911` |
| `BB_USER_NAME` | Supplies the online ID fallback name. | text; default `Hunter` | `engine/gpu/shim/net/bbnet_glue.cpp:78` |
| `BB_VBLANK_RELEARN` | Allows refresh-rate behavior to be relearned after variation. | 0 keeps variable for session; default enabled | `engine/gpu/shim/lantern_pacing.cpp:158` |

## Test aids

| Name | What it does | Values and default | Where read |
|---|---|---|---|
| `BB_AUTO_HUD_TEST` | Schedules HUD test actions after the world is up. | action list with seconds; default unset | `engine/gpu/shim/party/auto_hud.cpp:235` |
| `BB_CHALICE_GLYPH_BROWSER` | Enables the chalice glyph browser setting. | 0/1; default off | `engine/gpu/shim/bbport_settings.cpp:61` |
| `BB_DRIVER_CACHE_SAVE` | Controls saving the driver pipeline cache. | 0 disables; otherwise enabled; default (unclear) | `engine/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:632` |
| `BB_READBACK_LOG` | Writes a log of GPU buffer readbacks to the given path. | path; default off | `engine/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:138` |
| `BB_SF_MESH_GUARD` | Skips a UI mesh fill into a dynamic buffer the game got no memory for (guest +0x10b3406; NW-618899b guest crash); logs "Stability: sf mesh: fill into ... skipped". | 0 disables; default on | `engine/gpu/shim/bbport_crash_guards.cpp:875` |
