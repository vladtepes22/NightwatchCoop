// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: where the optional lighting passes hook into the frame. Every hook returns at once
// unless a lighting effect is on, so a player with all of them off pays one branch per call.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "bbport_settings.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/lighting_rules.h"
#include "video_core/renderer_vulkan/rt_lamp_rules.h"
#include "video_core/renderer_vulkan/vk_effect_shaders.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Vulkan {

namespace {
// The game's pixel shaders (pgm_hash): the sun shadow resolve (writes the shadow mask the contact
// shadow pass and soft sun shadows rework), the blur that follows it, and passes that are never lamps.
constexpr u64 ShadowResolveHash = 0xf55643de;
constexpr u64 ShadowBlurHash = 0x11ffdfb5;
constexpr u64 HdrPostHash = 0xccbf44a6;
constexpr u64 DirectionalTableHash = 0xd92266b9;
constexpr u64 SceneTargetHash = 0x32e47788;
constexpr u64 PointGridVertexHash = 0x8747a367;

/// The game's own light list as extra lamps: ray traced lamps only, which this build does not have.
bool GameLamps() {
    return false;
}

RtLamps::Family LampFamily(const GraphicsPipeline* pipeline, u64 hash, bool families, bool extended) {
    RtLamps::Family family = RtLamps::TileFamily(hash, families || extended);
    const bool claimed = hash == ShadowResolveHash || hash == ShadowBlurHash || hash == HdrPostHash ||
                         hash == DirectionalTableHash || hash == SceneTargetHash ||
                         hash == EffectShaders::SsaoHash || hash == EffectShaders::SsrHash ||
                         hash == EffectShaders::SsrCompositeHash || hash == EffectShaders::MotionBlurHash;
    if (!family.Lamp() && extended && !claimed && (pipeline->GetGraphicsKey().mrt_mask & 3) == 3 &&
        pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash == PointGridVertexHash) {
        family.unknown = true;
    }
    return family;
}

/// The first four buffer offsets of a draw, packed as the shaders read them (PushData::buf_offsets).
u32 PackedOffsets(const Shader::PushData& push) {
    static_assert(offsetof(Shader::PushData, buf_offsets) == 80);
    u32 word = 0;
    std::memcpy(&word, push.buf_offsets.data(), sizeof(word));
    return word;
}

u64 FragmentHash(const GraphicsPipeline* pipeline) {
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    return ps ? ps->pgm_hash : 0;
}
} // namespace

void Rasterizer::EffectsOnFrameEnd() {
    const auto& s = BbSettings::Get();
    bool on[BbSettings::EffectCount];
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        on[e] = s.effects[e].load(std::memory_order_relaxed);
    }
    effects_wanted = LightingRules::AnyWanted(on);
    if (!effects_wanted) {
        point_shadow->Drop();
    }
    point_shadow->OnFrameEnd();
    rt_shadow->OnFrameEnd();
    ssgi->OnFrameEnd();
    if (!on[BbSettings::EffectGi] && !rt_shadow->Wanted()) {
        ssgi->Release(); // the row was switched off: give the images back
    }
}

void Rasterizer::EffectsBeforeTargets(const GraphicsPipeline* pipeline) {
    if (!effects_wanted) {
        return;
    }
    const u64 hash = FragmentHash(pipeline);
    // The contact shadow / soft sun shadow pass runs on the finished sun shadow mask: before the next
    // draw that changes targets, but not before the game's own blur of it (which it then follows).
    contact_shadow->Flush(hash == ShadowBlurHash);
    if (!LampFamily(pipeline, hash, rt_shadow->Wanted(), GameLamps() && rt_shadow->Wanted()).Lamp()) {
        if (point_shadow->Pending()) {
            const auto& regs = stage_regs ? *stage_regs : liverpool->regs; // Regs(), inline in vk_rasterizer.cpp
            const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
            const bool writes_color =
                pipeline->GetGraphicsKey().mrt_mask != 0 &&
                regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable;
            const bool writes_depth =
                regs.depth_control.depth_enable && regs.depth_control.depth_write_enable;
            const auto lights = point_shadow->PendingAddresses();
            const auto is_light = [&](VAddr address) {
                return address != 0 && (address == lights[0] || address == lights[1]);
            };
            const u32 mrt = pipeline->GetGraphicsKey().mrt_mask;
            bool touches = !lights[0] || !lights[1] || (ps && ps->has_storage_images);
            for (u32 cb = 0; writes_color && !touches && cb < 8; ++cb) {
                touches = (mrt & (1u << cb)) && is_light(regs.color_buffers[cb].Address());
            }
            for (std::size_t i = 0; ps && !touches && i < ps->images.size(); ++i) {
                touches = is_light(ps->images[i].GetSharp(*ps).Address());
            }
            if (!writes_depth && !touches) {
                point_shadow->Kept();
                return;
            }
            point_shadow->Ended(hash);
        }
        const auto jitter = upscaler->Jitter();
        point_shadow->Flush(jitter[0] != 0.0f || jitter[1] != 0.0f);
    }
}

void Rasterizer::EffectsAfterTargets(const GraphicsPipeline* pipeline) {
    if (!effects_wanted) {
        return;
    }
    const auto& key = pipeline->GetGraphicsKey();
    if (std::popcount(key.mrt_mask) >= 5 && db_desc.first) {
        ssgi->OnGBufferDraw(db_desc.first, cb_descs[2].first, cb_descs[0].first);
    }
}

void Rasterizer::EffectsBlendedScene() {
    if (!effects_wanted) {
        return;
    }
    ssgi->OnOpaqueScene(cb_descs[0].first, db_desc.first, upscaler->Jitter());
}

void Rasterizer::EffectsBeforeUpscaler(u64) {
    if (!effects_wanted) {
        return;
    }
}

void Rasterizer::EffectsBeforeResources(const GraphicsPipeline*) {
    if (!effects_wanted) {
        return;
    }
}

vk::Rect2D Rasterizer::EffectTile(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline,
                                  bool is_indexed, u32 index_offset, u32 first_vertex) const {
    const vk::Rect2D scissor = DrawScissor(0);
    const auto& fetch = pipeline->GetFetchShader();
    const auto& v = vertex_binds;
    const auto position =
        std::ranges::find(v.attributes, 0u, &vk::VertexInputAttributeDescription2EXT::location);
    if (!fetch || position == v.attributes.end() || position->binding >= v.num_buffers) {
        return scissor;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    VAddr base = 0;
    u32 stride = 0;
    for (const auto& attribute : fetch->attributes) {
        if (attribute.semantic == 0) {
            const auto sharp = attribute.GetSharp(vs);
            base = sharp.base_address;
            stride = sharp.GetStride();
        }
    }
    if (!base || stride < 16) {
        return scissor;
    }
    const u32 index_size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
    const VAddr indices =
        is_indexed ? regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size : 0;
    if (is_indexed && (!memory->IsValidGpuMapping(indices, 0) ||
                       memory->ClampRangeSize(indices, 4 * index_size) != 4 * index_size)) {
        return scissor;
    }
    const auto& vp = regs.viewports[0];
    float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f;
    for (u32 i = 0; i < 4; ++i) {
        u32 index = i;
        if (is_indexed) {
            index = index_size == 2
                        ? reinterpret_cast<const u16*>(indices)[i]
                        : reinterpret_cast<const u32*>(indices)[i];
        }
        const VAddr at = base + u64(s64(index) + s64(s32(first_vertex))) * stride + position->offset;
        if (!memory->IsValidGpuMapping(at, 0) || memory->ClampRangeSize(at, 16) != 16) {
            return scissor;
        }
        const auto* p = reinterpret_cast<const float*>(at);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[3]) ||
            std::abs(p[3]) < 1e-8f) {
            return scissor;
        }
        const float x = p[0] / p[3] * (vp.xscale * target_scale[0]) +
                        (vp.xoffset * target_scale[0] + target_offset[0]);
        const float y = p[1] / p[3] * (vp.yscale * target_scale[1]) +
                        (vp.yoffset * target_scale[1] + target_offset[1]);
        lo_x = std::min(lo_x, x);
        lo_y = std::min(lo_y, y);
        hi_x = std::max(hi_x, x);
        hi_y = std::max(hi_y, y);
    }
    const s32 x0 = std::max(s32(std::floor(lo_x)), scissor.offset.x);
    const s32 y0 = std::max(s32(std::floor(lo_y)), scissor.offset.y);
    const s32 x1 = std::min(s32(std::ceil(hi_x)), scissor.offset.x + s32(scissor.extent.width));
    const s32 y1 = std::min(s32(std::ceil(hi_y)), scissor.offset.y + s32(scissor.extent.height));
    if (x1 <= x0 || y1 <= y0) {
        return scissor;
    }
    return {.offset = {x0, y0}, .extent = {u32(x1 - x0), u32(y1 - y0)}};
}

void Rasterizer::EffectsBeforeBind(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline,
                                   bool is_indexed, u32 index_offset, u32 first_vertex) {
    if (!pipeline->GetGraphicsKey().mrt_mask) {
        return;
    }
    const u64 hash = FragmentHash(pipeline);
    const auto family = LampFamily(pipeline, hash, rt_shadow->Wanted(), GameLamps() && rt_shadow->Wanted());
    if (family.Lamp()) {
        effect_tile = EffectTile(regs, pipeline, is_indexed, index_offset, first_vertex);
    }
    if (hash == ShadowBlurHash) {
        if (cb_descs[0].first) {
            const auto& blur = pipeline->GetStage(Shader::SwStage::Fragment);
            contact_shadow->OnBlurDraw(
                cb_descs[0].first, blur.images.empty() ? 0 : blur.images[0].GetSharp(blur).Address());
        }
    } else if (hash == ShadowResolveHash) {
        if (cb_descs[0].first && !buffer_infos.empty() && buffer_infos[0].buffer) {
            const bool has_ubo = buffer_infos.size() > 1 && buffer_infos[1].buffer;
            const bool has_map = image_infos.size() > 1 && image_infos[1].imageView;
            contact_shadow->OnShadowDraw(
                cb_descs[0].first,
                {.constants = buffer_infos[0].buffer,
                 .constants_offset = buffer_infos[0].offset,
                 .constants_range = buffer_infos[0].range,
                 .uniforms = has_ubo ? buffer_infos[1].buffer : vk::Buffer{},
                 .uniforms_offset = has_ubo ? buffer_infos[1].offset : 0,
                 .uniforms_range = has_ubo ? buffer_infos[1].range : 0,
                 .shadow_map = has_map ? image_infos[1].imageView : vk::ImageView{},
                 .shadow_layout = has_map ? image_infos[1].imageLayout : vk::ImageLayout::eUndefined,
                 .constants_offset_word = PackedOffsets(push_data),
                 .frame_index = shadow_frame++});
        }
    } else if (family.unknown) {
        if (cb_descs[0].first && cb_descs[1].first) {
            point_shadow->OnLightDraw(cb_descs[0].first, cb_descs[1].first,
                                      {.shader = hash, .tile = effect_tile});
        }
    } else if (family.count != 0) {
        const auto& ps = pipeline->GetStage(Shader::SwStage::Fragment);
        if ((pipeline->GetGraphicsKey().mrt_mask & 3) == 3 && cb_descs[0].first &&
            cb_descs[1].first && ps.buffers.size() >= 2 && !ps.buffers[0].IsSpecial() &&
            !ps.buffers[1].IsSpecial()) {
            const VAddr constants = ps.buffers[0].GetSharp(ps).base_address;
            const VAddr lights = ps.buffers[1].GetSharp(ps).base_address;
            const u64 light_bytes = u64(family.count) * family.stride * 4;
            if (memory->IsValidGpuMapping(constants, 0) && memory->ClampRangeSize(constants, 64) == 64 &&
                memory->IsValidGpuMapping(lights, 0) &&
                memory->ClampRangeSize(lights, light_bytes) == light_bytes) {
                point_shadow->OnLightDraw(cb_descs[0].first, cb_descs[1].first,
                                          {.constants = reinterpret_cast<const u32*>(constants),
                                           .lights = reinterpret_cast<const u32*>(lights),
                                           .count = family.count,
                                           .stride = family.stride,
                                           .radius_in_w = family.radius_in_w,
                                           .shader = hash,
                                           .tile = effect_tile});
            }
        }
    }
}

} // namespace Vulkan
