# Native+ HDR

Native+ (`hdr_mode=nativeplus`, the third HDR source next to Native and Expand) is LRF17's Native+ HDR
from [LanternEngine-NativePlusHDR](https://github.com/LRF17/LanternEngine-NativePlusHDR). The game's
final composite is replaced so its light stays in 16 bits through the color table, the upscalers and
frame generation, then is tone mapped to the display's peak by Vanilla+ (souperman9's, the default)
or PsychoV (RenoDX's PsychoV-30, MIT, `licenses/RenoDX-MIT.txt`). Native stays Nightwatch's default.

**Version here: Native+ 0.9.38.4** (LanternEngine-NativePlusHDR `ac06003`, branch
`lantern-Native+HDR-beta`). Nightwatch 0.2.6 beta 1 had Native+ of 2026-10-25 (then named HDR16,
`f942b35`); this update brings everything since, Native+ code only (none of Lantern 0.9.38's own
changes).

## Settings compatibility

Nothing to migrate by hand: the engine reads every older `bbport.ini`.

- `hdr_mode=psychov30` (HDR16's name) is read as `nativeplus` and written back as `nativeplus`;
  the launcher's Display page shows it as Native+.
- `hdr_tonemapper`: 5 Vanilla+ (default), 4 PsychoV. 7 (Native+ 0.9.38.2's PsychoV) reads as 4 with
  both sliders at 100 %; 0, 2, 3 and 6 (Vanilla and the old hybrids) read as Vanilla+. Stored values
  are never renumbered.
- New: `hdr_vplus_highlights` and `hdr_hue_correction` (0..100, default 0), PsychoV's Vanilla+
  Highlights and Hue Correction sliders in the in-game menu.
- Gone: `hdr_gamut`, `hdr_lut_bridge`, `hdr_compression`, `hdr_gamut_compression` (PsychoV fits
  BT.2020 with RenoDX's defaults); a file that has them loads and drops them.

## What belongs to Native+

Files only Native+ uses (taken as they are from LanternEngine-NativePlusHDR on each update):

- `engine/gpu/shadps4/video_core/host_shaders/hdr/` (all of it) and `host_shaders/hdr_upscale_transfer.comp`
- `engine/gpu/shadps4/video_core/renderer_vulkan/vk_hdr_channel.{h,cpp}`, `bb_tonemap_variants.h`
- `engine/docs/hdr-tools/` (the numerical checks behind the shaders)

Nightwatch files with Native+ hunks (merged by hand on each update; Native+ code there sits behind
`startup_psychov30` / `HdrChannel` checks):

- in-game menu and settings: `engine/gpu/shim/bbport_overlay.cpp` (`PsychoSection`),
  `bbport_settings.{h,cpp}`, `engine/tests/test_settings.cpp`, `shim/engine/graphics.cpp`
- renderer: `vk_rasterizer.{h,cpp}`, `vk_presenter.cpp`, `vk_temporal_upscaler.{h,cpp}`,
  `vk_frame_generation.{h,cpp}`, `vk_graphics_pipeline.cpp`, `vk_pipeline_cache.cpp`,
  `vk_pipeline_serialization.cpp`, `host_shaders/hdr_output.frag`, `engine/gpu/CMakeLists.txt`
- launcher: `Core/Choices.cs` (`HdrMode`, `HdrToneMapper`), `Views/DisplayPage.cs` (HDR rows)

## Updates

Each Native+ update for Nightwatch comes as one commit on top of Nightwatch's `main`, in
[LRF17/NightwatchCoop-NativePlusHDR](https://github.com/LRF17/NightwatchCoop-NativePlusHDR)
(branch `Native+HDR`), with the Native+ version in its title. Taking it is a cherry-pick:

```
git fetch https://github.com/LRF17/NightwatchCoop-NativePlusHDR Native+HDR
git cherry-pick FETCH_HEAD
```

The commit's own delta is computed from LanternEngine-NativePlusHDR (old Native+ on its Lantern
version against new Native+ on its Lantern version, Native+ hunks only) and merged three-way onto
Nightwatch's files, so Nightwatch's own changes to those files stay.
