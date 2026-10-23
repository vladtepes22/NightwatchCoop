// SPDX-License-Identifier: GPL-2.0-or-later
// The map's renderer (map_vector.h). Adapted from Hunter's Atlas source/compact/map_vector.cpp:
// its own pipeline (Atlas patches ImGui's Vulkan backend for one), Vulkan calls through the
// engine's dispatcher, the reveal flags and paths are the caller's, and replaced GPU buffers are
// retired a few frames later instead of waiting for the device.
#include "map_vector.h"

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/host_shaders/map_vector_frag.h"
#include "video_core/host_shaders/map_vector_vert.h"

#include "core.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"

#include <chrono>
#include <cstring>
#include <future>
#include <memory>

namespace BbMapVector {
namespace {
using namespace Atlas2;

constexpr size_t kLimit = 256 * 1024 * 1024;
constexpr int kRetireFrames = 8;  // more than the frames in flight

struct Group {
    uint32_t first, count;
    std::vector<int> required;  // reveal regions
};
struct Mesh {
    uint32_t first = 0, count = 0, vertex = 0;
    std::vector<Group> groups;
};
struct Buffers {
    std::vector<unsigned char> vertices, indices;
};
struct Layer {
    float anchor, low, high;
    Mesh floors, outlines, walls, interiors;
};
struct Hatch {
    Mesh cut, interior;
};
struct Area {
    std::string region;
    fs::path root;
    Json manifest;
    Grid grid;
    Buffers buffers;
    std::vector<Layer> layers;
    std::array<float, 4> bounds;
    int primary;
    uint64_t serial = 0;
};
struct Hatches {
    std::string region;
    int lod = -1;
    uint64_t serial = 0;
    Buffers buffers;
    std::vector<Hatch> layers;
};
struct GpuBuffer {
    VkBuffer vertices{}, indices{};
    VkDeviceMemory vm{}, im{};
    uint64_t serial = 0;
};

fs::path compiled_dir, parchment_path;
std::shared_ptr<Area> area;
std::shared_ptr<Hatches> hatches;
std::future<std::shared_ptr<Area>> area_job;
std::future<std::shared_ptr<Hatches>> hatch_job;
std::string requested, area_target, failed, error;
int previous = -1, manual = -1;
bool prior_valid = false;
float last_x = 0, last_z = 0;
uint64_t serial = 0;
// The pipeline: ImGui's vertex layout, render state and descriptor set layouts (set 0 a sampled
// image, set 1 a sampler, both fragment-only: identically defined, so ImGui's texture descriptors
// bind with it), the map's shaders and push constants.
VkDevice device{};
VkPhysicalDevice physical{};
VkDescriptorSetLayout image_layout{}, sampler_layout{};
VkDescriptorPool pool{};
VkSampler linear{};
VkDescriptorSet sampler{};
VkPipeline pipeline{};
VkPipelineLayout layout{};
GpuBuffer base_gpu, hatch_gpu;
std::vector<std::pair<GpuBuffer, int>> retired;  // with the frame they were replaced in
std::unique_ptr<ImTextureData> parchment, white;
int active_layer = -1;
struct Frame {
    ImVec2 center;
    float x, z, zoom, scale, opacity;
    bool shading, fog;
    std::vector<bool> flags;
} frame;

auto& Vk() {
    return VULKAN_HPP_DEFAULT_DISPATCHER;
}

bool Busy() {
    return area_job.valid() || hatch_job.valid();
}
template <class T>
bool Ready(std::future<T>& job) {
    return job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// "A3Z1", raw size, zlib(msgpack); the hash is the manifest's.
std::vector<unsigned char> Inflate(const fs::path& p, const std::string& hash) {
    const auto b = Read(p, kLimit);
    if (b.size() < 8 || std::memcmp(b.data(), "A3Z1", 4) || Hash(b) != hash) throw std::runtime_error("compiled map integrity");
    const auto size = U32(b.data() + 4);
    if (size > kLimit) throw std::runtime_error("compiled map size");
    std::vector<unsigned char> out(size);
    uLongf n = size;
    uLong used = uLong(b.size() - 8);
    if (uncompress2(out.data(), &n, b.data() + 8, &used) != Z_OK || n != size || used != b.size() - 8)
        throw std::runtime_error("compiled map decode");
    return out;
}

Json Blob(const Area& a, const std::string& name) {
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") != std::string::npos)
        throw std::runtime_error("compiled path");
    return Json::from_msgpack(Inflate(a.root / name, a.manifest.at("files").at(name)));
}

Mesh ParseMesh(Json& j, Buffers& out) {
    Mesh mesh;
    const auto& v = j.at("vertices").get_binary();
    const auto& i = j.at("indices").get_binary();
    if (v.size() % 20 || i.size() % 4 || out.vertices.size() + v.size() > kLimit || out.indices.size() + i.size() > kLimit)
        throw std::runtime_error("compiled mesh bounds");
    mesh.vertex = uint32_t(out.vertices.size() / 20);
    mesh.first = uint32_t(out.indices.size() / 4);
    mesh.count = uint32_t(i.size() / 4);
    if (mesh.count % 3) throw std::runtime_error("compiled triangle count");
    for (size_t k = 0; k < i.size(); k += 4)
        if (U32(i.data() + k) >= v.size() / 20) throw std::runtime_error("compiled vertex index");
    uint32_t end = 0;
    for (const auto& g : j.at("groups")) {
        Group group{g.at(0), g.at(1), g.at(2).get<std::vector<int>>()};
        if (group.first != end || group.count > mesh.count - end || group.required.size() > 256)
            throw std::runtime_error("compiled draw range");
        for (int r : group.required)
            if (r < -1 || r > 255) throw std::runtime_error("compiled reveal index");
        end += group.count;
        mesh.groups.push_back(std::move(group));
    }
    if (end != mesh.count) throw std::runtime_error("compiled mesh coverage");
    out.vertices.insert(out.vertices.end(), v.begin(), v.end());
    out.indices.insert(out.indices.end(), i.begin(), i.end());
    return mesh;
}

std::shared_ptr<Area> ReadArea(const fs::path& dir, std::string region, uint64_t id) {
    auto index = Json::parse(Read(dir / "atlas2-index.json", 1024 * 1024));
    if (index.at("schema") != 1 || index.at("format") != "compiled-vector-v1") throw std::runtime_error("compiled index format");
    Json entry;
    for (const auto& e : index.at("areas"))
        if (e.at("region_id") == region) entry = e;
    if (entry.is_null()) throw std::runtime_error("no compiled map for " + region);
    const std::string label = entry.at("area");
    if (label.size() != 6 || label.rfind("Area", 0) || label.find_first_not_of("0123456789", 4) != std::string::npos)
        throw std::runtime_error("compiled area name");
    auto result = std::make_shared<Area>();
    result->region = region;
    result->serial = id;
    result->root = dir / label;
    const auto bytes = Read(result->root / "manifest.json", 1024 * 1024);
    if (Hash(bytes) != entry.at("manifest_sha256").get<std::string>()) throw std::runtime_error("compiled manifest integrity");
    result->manifest = Json::parse(bytes);
    if (result->manifest.at("schema") != 4 || result->manifest.at("region_id") != region)
        throw std::runtime_error("compiled manifest schema");
    result->bounds = result->manifest.at("bounds").get<std::array<float, 4>>();
    for (float v : result->bounds)
        if (!std::isfinite(v)) throw std::runtime_error("compiled bounds");
    result->primary = result->manifest.at("primary");
    result->grid.FromJson(Blob(*result, "grid.a3z"));
    auto base = Blob(*result, "base.a3z");
    if (base.at("schema") != 1 || base.at("layers").size() != result->grid.anchors.size())
        throw std::runtime_error("compiled layer identity");
    for (auto& l : base["layers"]) {
        Layer layer{l.at("anchor"), l.at("low"), l.at("high"), {}, {}, {}, {}};
        layer.floors = ParseMesh(l["floors"], result->buffers);
        layer.outlines = ParseMesh(l["outlines"], result->buffers);
        layer.walls = ParseMesh(l["walls"], result->buffers);
        layer.interiors = ParseMesh(l["interiors"], result->buffers);
        result->layers.push_back(std::move(layer));
    }
    if (result->primary < 0 || result->primary >= int(result->layers.size())) throw std::runtime_error("compiled primary floor");
    return result;
}

std::shared_ptr<Hatches> ReadHatches(std::shared_ptr<Area> source, int lod, uint64_t id) {
    auto j = Blob(*source, "hatch-" + std::to_string(lod) + ".a3z");
    auto result = std::make_shared<Hatches>();
    result->region = source->region;
    result->lod = lod;
    result->serial = id;
    if (j.at("schema") != 1 || j.at("layers").size() != source->layers.size()) throw std::runtime_error("compiled hatch identity");
    for (auto& l : j["layers"]) {
        Hatch h;
        h.cut = ParseMesh(l["cut"], result->buffers);
        h.interior = ParseMesh(l["interior"], result->buffers);
        result->layers.push_back(std::move(h));
    }
    return result;
}

void Pump() {
    if (Ready(area_job)) {
        try {
            auto next = area_job.get();
            if (next->region == requested) {
                area = std::move(next);
                hatches.reset();
                previous = -1;
                prior_valid = false;
                error.clear();
            }
        } catch (const std::exception& e) {
            failed = area_target;
            if (failed == requested) {
                error = e.what();
                std::printf("Map: %s\n", e.what());
            }
        }
    }
    if (Ready(hatch_job)) {
        try {
            auto next = hatch_job.get();
            if (area && next->region == area->region) {
                hatches = std::move(next);
                error.clear();
            }
        } catch (const std::exception& e) {
            error = e.what();
            std::printf("Map: %s\n", e.what());
        }
    }
}

void RequestHatches(int lod) {
    Pump();
    if (!area || area->region != requested || (hatches && hatches->region == area->region && hatches->lod == lod) || Busy()) return;
    auto source = area;
    const uint64_t id = ++serial;
    hatch_job = std::async(std::launch::async, [source, lod, id] { return ReadHatches(source, lod, id); });
}

void Check(VkResult value) {
    if (value != VK_SUCCESS) throw std::runtime_error("map GPU allocation " + std::to_string(value));
}

uint32_t Memory(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties props;
    Vk().vkGetPhysicalDeviceMemoryProperties(physical, &props);
    constexpr VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("map GPU memory");
}

void Free(GpuBuffer& b) {
    if (!device) return;
    if (b.vertices) Vk().vkDestroyBuffer(device, b.vertices, nullptr);
    if (b.indices) Vk().vkDestroyBuffer(device, b.indices, nullptr);
    if (b.vm) Vk().vkFreeMemory(device, b.vm, nullptr);
    if (b.im) Vk().vkFreeMemory(device, b.im, nullptr);
    b = {};
}

void UploadOne(VkBuffer& buffer, VkDeviceMemory& memory, const std::vector<unsigned char>& bytes, VkBufferUsageFlags usage) {
    if (bytes.empty()) return;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes.size();
    ci.usage = usage;
    Check(Vk().vkCreateBuffer(device, &ci, nullptr, &buffer));
    VkMemoryRequirements req;
    Vk().vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = Memory(req.memoryTypeBits);
    Check(Vk().vkAllocateMemory(device, &ai, nullptr, &memory));
    Check(Vk().vkBindBufferMemory(device, buffer, memory, 0));
    void* data;
    Check(Vk().vkMapMemory(device, memory, 0, bytes.size(), 0, &data));
    std::memcpy(data, bytes.data(), bytes.size());
    Vk().vkUnmapMemory(device, memory);
}

void Upload(GpuBuffer& target, const Buffers& source, uint64_t id) {
    if (target.serial == id) return;
    GpuBuffer next;
    try {
        UploadOne(next.vertices, next.vm, source.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        UploadOne(next.indices, next.im, source.indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        next.serial = id;
        // Frames in flight may still read the old buffers.
        if (target.vertices || target.indices) retired.emplace_back(target, ImGui::GetFrameCount());
        target = next;
    } catch (...) {
        Free(next);
        throw;
    }
}

void FreeRetired() {
    const int now = ImGui::GetFrameCount();
    std::erase_if(retired, [now](auto& r) {
        if (now - r.second < kRetireFrames) return false;
        Free(r.first);
        return true;
    });
}

std::unique_ptr<ImTextureData> MakeTexture(int w, int h) {
    auto t = std::make_unique<ImTextureData>();
    t->Create(ImTextureFormat_RGBA32, w, h);
    t->UseColors = true;
    static int unique = 4000000;
    t->UniqueID = unique++;
    return t;
}

void Textures() {
    if (white) return;
    const auto b = Read(parchment_path, 64 * 1024 * 1024 + 12);
    if (b.size() < 12 || std::memcmp(b.data(), "BBRA", 4)) throw std::runtime_error("map parchment");
    const int w = int(U32(b.data() + 4)), h = int(U32(b.data() + 8));
    if (w < 1 || h < 1 || w > 4096 || h > 4096 || b.size() != 12 + size_t(w) * h * 4) throw std::runtime_error("map parchment size");
    auto p = MakeTexture(w, h);
    std::memcpy(p->GetPixels(), b.data() + 12, b.size() - 12);
    auto blank = MakeTexture(2, 2);
    std::memset(blank->GetPixels(), 255, 16);
    ImGui::RegisterUserTexture(p.get());
    ImGui::RegisterUserTexture(blank.get());
    parchment = std::move(p);
    white = std::move(blank);
}

// Push constants of the map's vertex shader (map_vector.vert).
struct Constants {
    float view[4], data[4], paint[4], params[4];
};

bool Allowed(const Group& group) {
    if (!frame.fog) return true;
    for (int r : group.required)
        if (r < 0 || r >= int(frame.flags.size()) || !frame.flags[r]) return false;
    return true;
}

// kind 0: flat colour, 1: parchment floors shaded by height, 2: strokes extruded by `width`.
void RenderMesh(VkCommandBuffer cmd, const Mesh& mesh, const GpuBuffer& buffer, Constants constants, int kind,
                std::array<float, 4> paint, float width = 0, int wear = 0) {
    if (!mesh.count || !buffer.vertices) return;
    constants.data[3] = float(kind);
    std::copy(paint.begin(), paint.end(), constants.paint);
    constants.params[2] = width;
    constants.params[3] = float(wear);
    const auto texture = kind == 1 ? parchment->GetTexID() : white->GetTexID();
    const auto descriptor = (VkDescriptorSet)(uintptr_t)texture;
    Vk().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptor, 0, nullptr);
    const VkDeviceSize zero = 0;
    Vk().vkCmdBindVertexBuffers(cmd, 0, 1, &buffer.vertices, &zero);
    Vk().vkCmdBindIndexBuffer(cmd, buffer.indices, 0, VK_INDEX_TYPE_UINT32);
    Vk().vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
    if (!frame.fog) {
        Vk().vkCmdDrawIndexed(cmd, mesh.count, 1, mesh.first, int32_t(mesh.vertex), 0);
        return;
    }
    for (const auto& g : mesh.groups)
        if (Allowed(g)) Vk().vkCmdDrawIndexed(cmd, g.count, 1, mesh.first + g.first, int32_t(mesh.vertex), 0);
}

void Callback(const ImDrawList*, const ImDrawCmd* command) {
    try {
        if (!area || active_layer < 0 || active_layer >= int(area->layers.size()) || !parchment ||
            parchment->Status != ImTextureStatus_OK || white->Status != ImTextureStatus_OK)
            return;
        if (!pipeline) return;
        auto* state = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
        if (!state) return;
        FreeRetired();
        Upload(base_gpu, area->buffers, area->serial);
        if (hatches && hatches->region == area->region) Upload(hatch_gpu, hatches->buffers, hatches->serial);

        const auto* dd = ImGui::GetDrawData();
        const auto pos = dd->DisplayPos, size = dd->DisplaySize, scale = dd->FramebufferScale;
        if (size.x <= 0 || size.y <= 0) return;
        const auto clip = command->ClipRect;
        const float x0 = std::clamp((clip.x - pos.x) * scale.x, 0.f, size.x * scale.x);
        const float y0 = std::clamp((clip.y - pos.y) * scale.y, 0.f, size.y * scale.y);
        const float x1 = std::clamp((clip.z - pos.x) * scale.x, 0.f, size.x * scale.x);
        const float y1 = std::clamp((clip.w - pos.y) * scale.y, 0.f, size.y * scale.y);
        if (x1 <= x0 || y1 <= y0) return;
        const auto cmd = state->CommandBuffer;
        const VkRect2D scissor{{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        Vk().vkCmdSetScissor(cmd, 0, 1, &scissor);
        Vk().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        Vk().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &sampler, 0, nullptr);

        // Vertices are (x, -z): screen = center + (x - cx, cz - z) * zoom.
        const Constants c{{2 * frame.zoom / size.x, 2 * frame.zoom / size.y,
                           2 * (frame.center.x - frame.x * frame.zoom - pos.x) / size.x - 1,
                           2 * (frame.center.y + frame.z * frame.zoom - pos.y) / size.y - 1},
                          {(area->bounds[0] + area->bounds[2]) * .5f, (area->bounds[1] + area->bounds[3]) * .5f,
                           std::max(area->bounds[2] - area->bounds[0], area->bounds[3] - area->bounds[1]), 0},
                          {},
                          {2 / size.x, 2 / size.y, 0, 0}};
        const auto& active = area->layers[active_layer];
        float low = active.anchor, high = low;  // height shading range: the floors near the active one
        for (const auto& l : area->layers)
            if (std::abs(l.anchor - active.anchor) <= 6) {
                low = std::min(low, l.low);
                high = std::max(high, l.high);
            }
        const auto colour = [&](int r, int g, int b, int a) {
            return std::array<float, 4>{r / 255.f, g / 255.f, b / 255.f, a / 255.f * frame.opacity};
        };
        // Enclosed interiors (other floors first), their hatching, the other floors faintly, the
        // cut hatching along the active floor's walls, then the active floor, outlines and walls.
        for (int pass = 0; pass < 2; ++pass)
            for (size_t li = 0; li < area->layers.size(); ++li)
                if ((int(li) == active_layer) == (pass == 1))
                    RenderMesh(cmd, area->layers[li].interiors, base_gpu, c, 0, pass ? colour(53, 48, 39, 255) : colour(44, 41, 34, 255));
        const bool hatched = hatches && hatches->region == area->region && hatch_gpu.serial == hatches->serial;
        const float thin = std::clamp(.72f * frame.scale, .6f, 1.3f);
        if (hatched)
            for (size_t li = 0; li < hatches->layers.size(); ++li)
                RenderMesh(cmd, hatches->layers[li].interior, hatch_gpu, c, 2, colour(135, 119, 90, 0), thin,
                           int(li) == active_layer ? 1 : 2);
        for (size_t li = 0; li < area->layers.size(); ++li)
            if (int(li) != active_layer) {
                const auto& l = area->layers[li];
                const bool near = std::abs(l.anchor - active.anchor) <= 6;
                RenderMesh(cmd, l.floors, base_gpu, c, 1, {low, high, (near ? 38.f : 27.f) / 255 * frame.opacity, frame.shading ? 1.f : 0.f});
                RenderMesh(cmd, l.outlines, base_gpu, c, 2, colour(213, 203, 177, near ? 62 : 49), .7f * frame.scale);
            }
        if (hatched) RenderMesh(cmd, hatches->layers[active_layer].cut, hatch_gpu, c, 2, colour(166, 160, 143, 0), thin, 1);
        RenderMesh(cmd, active.floors, base_gpu, c, 1, {low, high, frame.opacity, frame.shading ? 1.f : 0.f});
        RenderMesh(cmd, active.outlines, base_gpu, c, 2, colour(204, 193, 163, 220), .85f * frame.scale);
        RenderMesh(cmd, active.walls, base_gpu, c, 2, colour(15, 18, 19, 255),
                   std::clamp(frame.zoom * .13f, 2.5f * frame.scale, 5.f * frame.scale));
    } catch (const std::exception& e) {
        error = e.what();
    }
}

VkShaderModule Shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule module{};
    Check(Vk().vkCreateShaderModule(device, &ci, nullptr, &module));
    return module;
}

VkDescriptorSetLayout SetLayout(VkDescriptorType type) {
    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = type;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    VkDescriptorSetLayout result{};
    Check(Vk().vkCreateDescriptorSetLayout(device, &ci, nullptr, &result));
    return result;
}

void CreateGpu(VkFormat format) {
    image_layout = SetLayout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    sampler_layout = SetLayout(VK_DESCRIPTOR_TYPE_SAMPLER);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = -1000;
    si.maxLod = 1000;
    si.maxAnisotropy = 1.0f;
    Check(Vk().vkCreateSampler(device, &si, nullptr, &linear));
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    Check(Vk().vkCreateDescriptorPool(device, &pi, nullptr, &pool));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &sampler_layout;
    Check(Vk().vkAllocateDescriptorSets(device, &ai, &sampler));
    VkDescriptorImageInfo image{};
    image.sampler = linear;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sampler;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    Vk().vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Constants)};
    const VkDescriptorSetLayout sets[] = {image_layout, sampler_layout};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = sets;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    Check(Vk().vkCreatePipelineLayout(device, &li, nullptr, &layout));

    const VkShaderModule vert = Shader(MAP_VECTOR_VERT, sizeof(MAP_VECTOR_VERT));
    const VkShaderModule frag = Shader(MAP_VECTOR_FRAG, sizeof(MAP_VECTOR_FRAG));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[0].pName = stages[1].pName = "main";
    // The baker's vertex (x, -z, u, v, colour) is ImDrawVert shaped.
    static_assert(sizeof(ImDrawVert) == 20);
    const VkVertexInputBindingDescription binding{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(ImDrawVert, pos))},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(ImDrawVert, uv))},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(ImDrawVert, col))},
    };
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 3;
    vertex.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // ImGui's blending (premultiplied destination alpha).
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex;
    ci.pInputAssemblyState = &assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blending;
    ci.pDynamicState = &dynamic;
    ci.layout = layout;
    const VkResult result = Vk().vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    Vk().vkDestroyShaderModule(device, vert, nullptr);
    Vk().vkDestroyShaderModule(device, frag, nullptr);
    Check(result);
}

} // namespace

bool InitGpu(VkDevice gpu, VkPhysicalDevice physical_device, VkFormat format) {
    ReleaseGpu();
    device = gpu;
    physical = physical_device;
    try {
        CreateGpu(format);
        return true;
    } catch (const std::exception& e) {
        std::printf("Map: %s\n", e.what());
        ReleaseGpu();
        return false;
    }
}

void Configure(const std::filesystem::path& compiled, const std::filesystem::path& parchment_file) {
    compiled_dir = compiled;
    parchment_path = parchment_file;
}

bool Configured() {
    return !compiled_dir.empty();
}

bool Load(const std::string& region) {
    if (!Configured() || region.empty()) return false;
    Pump();
    if (requested != region) {
        requested = region;
        previous = -1;
        prior_valid = false;
        manual = -1;
    }
    if (area && area->region == region) return true;
    if (failed == region || Busy()) return false;
    area_target = region;
    const uint64_t id = ++serial;
    area_job = std::async(std::launch::async, [dir = compiled_dir, region, id] { return ReadArea(dir, region, id); });
    return false;
}

int Select(const std::string& region, float x, float y, float z, bool valid, bool continuity) {
    if (!valid || !Load(region)) {
        prior_valid = false;
        previous = -1;
        return -1;
    }
    const bool keep = continuity && prior_valid && std::hypot(x - last_x, z - last_z) < 20;
    const int selected = area->grid.Select(x, y, z, keep ? previous : -1, keep ? .35f : 0.f, manual);
    if (continuity) {
        previous = selected;
        last_x = x;
        last_z = z;
        prior_valid = selected >= 0;
    }
    return selected;
}

int Primary(const std::string& region) {
    return Load(region) ? area->primary : -1;
}

int Floors(const std::string& region) {
    return Load(region) ? int(area->layers.size()) : 0;
}

bool FloorHeights(const std::string& region, int floor, float* anchor, float* low, float* high) {
    if (!Load(region) || floor < 0 || floor >= int(area->layers.size())) return false;
    const Layer& l = area->layers[floor];
    *anchor = l.anchor;
    *low = l.low;
    *high = l.high;
    return true;
}

void SetManualFloor(int floor) {
    manual = area && floor >= 0 && floor < int(area->layers.size()) ? floor : -1;
    previous = -1;
    prior_valid = false;
}

int ManualFloor() {
    return manual;
}

void Draw(ImDrawList* draw, const std::string& region, int floor, ImVec2 center, float cx, float cz, float zoom, float opacity,
          bool shading, float ui_scale, const std::vector<bool>* revealed) {
    if (!std::isfinite(cx) || !std::isfinite(cz) || !std::isfinite(zoom) || zoom <= 0 || !std::isfinite(ui_scale) ||
        ui_scale <= 0 || !Load(region) || floor < 0 || floor >= int(area->layers.size()))
        return;
    try {
        Textures();
        // The hatch level whose spacing suits this zoom (Atlas's CutCache::Level).
        const int lod = std::clamp(int(std::lround(std::log(5.5f * 1.41421356237f * ui_scale / std::max(.0001f, zoom * .125f)) /
                                                   std::log(1.5f))),
                                   2, 17);
        RequestHatches(lod);
        frame = {center, cx, cz, zoom, ui_scale, std::clamp(opacity, 0.f, 1.f), shading, revealed != nullptr,
                 revealed ? *revealed : std::vector<bool>{}};
        active_layer = floor;
        draw->AddCallback(Callback, nullptr);
        draw->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
    } catch (const std::exception& e) {
        error = e.what();
    }
}

std::string Error() {
    return error;
}

void ReleaseGpu() {
    if (device) {
        Vk().vkDeviceWaitIdle(device);
        Free(base_gpu);
        Free(hatch_gpu);
        for (auto& r : retired) Free(r.first);
        retired.clear();
        if (pipeline) Vk().vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) Vk().vkDestroyPipelineLayout(device, layout, nullptr);
        if (pool) Vk().vkDestroyDescriptorPool(device, pool, nullptr);  // frees `sampler`
        if (linear) Vk().vkDestroySampler(device, linear, nullptr);
        if (image_layout) Vk().vkDestroyDescriptorSetLayout(device, image_layout, nullptr);
        if (sampler_layout) Vk().vkDestroyDescriptorSetLayout(device, sampler_layout, nullptr);
    }
    device = {};
    physical = {};
    pipeline = {};
    layout = {};
    pool = {};
    linear = {};
    sampler = {};
    image_layout = sampler_layout = {};
}

void Shutdown() {
    if (area_job.valid()) try {
            (void)area_job.get();
        } catch (...) {
        }
    if (hatch_job.valid()) try {
            (void)hatch_job.get();
        } catch (...) {
        }
    ReleaseGpu();
    for (auto* t : {&parchment, &white})
        if (*t && (*t)->GetTexID() == ImTextureID_Invalid && !(*t)->BackendUserData) {
            ImGui::UnregisterUserTexture(t->get());
            t->reset();
        }
    area.reset();
    hatches.reset();
    requested.clear();
    failed.clear();
    previous = -1;
    prior_valid = false;
}

} // namespace BbMapVector
