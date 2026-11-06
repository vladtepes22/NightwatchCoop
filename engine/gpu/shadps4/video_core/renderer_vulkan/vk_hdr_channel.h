// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport HDR transport (docs/HDR.md):
// Mode 5 keeps the post-composite chain in RGBA16F and adds the composite's HDR delta directly
// to the game's target. PsychoV30 (scene) runs there, including the Vanilla shadow match.
// Legacy modes 3/4 instead replay the composite into a separate gain/luminance side channel;
// the final pass applies those gains to the finished SDR frame outside the detected UI.

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;

class HdrChannel {
public:

    // ---- BB_HDR_TONEMAP=5: the 16-bit chain (docs/HDR.md, "Chantier 16 bits") ----------------
    // The game's 8-bit targets after its composite (1920x1080 RGBA8: the composite's target, its
    // copy for the AA, and the display buffers) become RGBA16F at the port's output-size images
    // (scene proxies, UI and display images: only with a scaled output). The composite draw is
    // followed by bbport's, which adds the HDR part (hdr/bb_composite_hdr.frag, BB_OUTPUT_DELTA),
    // so the frame carries light above the game's white to the final pass.
    static bool Native16();
    /// The in-game menu's 16-bit probe for the chain's passes (bb_hdr.z): 1..5, else 0 (probe 6
    /// is the final pass's, set by the presenter).
    static u32 ChainProbe();
    /// Mode 5 with the startup-patched render size (BB_RENDER_RES, "Live resolution changes"
    /// off): the game's targets are already at the render size and the scene has no proxies, so
    /// the chain's targets get same-size RGBA16F proxies of their own (SceneTargets::OwnSize).
    static bool Native16Fixed();
    /// Mode 5 and the port's output-size images in use (set by the scene proxies, each frame).
    static bool Native16Active();
    static void SetNative16Active(bool active);
    /// GPU thread: the color target of a draw with this pixel shader (guest address, size,
    /// format): composite variants and the shaders of the chain mark their 1920x1080 RGBA8
    /// target as 16-bit from the next frame on.
    static void NoteTarget(u64 ps_hash, VAddr address, u32 width, u32 height, vk::Format format);
    /// Mode 5 with the startup patch: the destination of a copy of a chain target (the game
    /// copies its finished image for its color grading with a compute copy, not a draw) joins
    /// the chain from the next frame on, so the copy is done between RGBA16F proxies.
    static void NoteCopyTarget(VAddr address, u32 width, u32 height, vk::Format format);
    /// Whether this guest color target is drawn as RGBA16F (pipeline formats, proxies, views).
    static bool Upgraded(VAddr address, u32 width, u32 height, vk::Format format);
    /// The display buffers (the game's last copy) when the port draws them at output size.
    static void SetDisplayRedirect(bool redirect);
    static bool DisplayUpgraded(VAddr address, vk::Format format);
    /// The format the game's display copy writes through (its 8-bit attachment): sRGB when the
    /// copy's output is linear (encoded by the attachment), UNORM when it is already encoded.
    static vk::Format DisplayAttachmentFormat();
    static constexpr vk::Format Native16Format = vk::Format::eR16G16B16A16Sfloat;
    /// The game's ColorGrading pass (a color table), replaced in mode 5; its strength (in-game
    /// menu, Scene grading) goes to the shader in bb_hdr.w (hdr/bb_color_grading.frag).
    static constexpr u64 ColorGradingHash = 0xed4c7668;
    /// The game's second glow, added after its tone mapping (a blur of the 960x540 glow targets,
    /// additive); in mode 5 weighted by the in-game Bloom slider (blend constant).
    static constexpr u64 GlowAddHash = 0x8c08444c;
    /// Mode 5: bbport's version of a game shader of the chain (glow source, display copy) with
    /// the game shader's bindings and locations, or empty to keep the game's.
    static std::vector<u32> ChainReplacement(u64 pgm_hash, std::span<const u32> game);
    /// Mode 5 test (in-game menu, "Log the 16-bit chain"): one frame, from a display copy to the
    /// next, where every access to the chain's targets is logged (HDR trace lines).
    static bool Trace();
    /// GPU thread, at each display copy: starts or ends the traced frame.
    static void OnDisplayCopy();
    /// Whether a texture cache image is one of the chain's 16-bit targets.
    static bool ChainImage(VAddr address, u32 width, u32 height, vk::Format format);

    /// Generic pass: called with the game's SPIR-V of every shader bbport compiles or loads. For
    /// a composite variant, builds hdr/bb_composite_hdr.frag with that variant's interface
    /// (descriptor bindings, input locations, constant buffer) and options.
    static void NoteGameShader(u64 pgm_hash, std::span<const u32> spv);
    /// Generic pass: the HDR fragment shader for this composite variant (null if none).
    static vk::ShaderModule CompositeModule(const Instance& instance, u64 pgm_hash);
    /// Peak luminance in nits (BB_HDR_PEAK_NITS, 1000 by default, 200..10000).
    static float PeakNits();
    /// Fraction of the game's white below which the game's curve is kept (BB_HDR_KNEE, 0.6).
    static float Knee();
    /// Effective live tone mapper: Neutwo (0), PsychoV30 hybrid (1), Vanilla (2), scene (3).
    static u32 Tonemapper();
    /// Mode 5 showing the game's own picture (the menu's preset Off, tone mapper Vanilla, or
    /// Compare on SDR full screen): the chain's HDR changes are left out (no composite delta,
    /// the game's bloom, second glow and color table) so the 16-bit chain draws the game's own.
    static bool VanillaChain();
    /// BB_HDR_DEBUG: 0 off, 1 the gain as a heat map, 2 the pixels taken as UI in red.
    static u32 Debug();

    /// The game retains its original 128-byte prefix; only mode 5 needs the HDR extension.
    static u32 PushConstantSize() { return Native16() ? 176u : 128u; }
};

} // namespace Vulkan
