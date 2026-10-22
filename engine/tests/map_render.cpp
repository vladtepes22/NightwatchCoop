// SPDX-License-Identifier: GPL-2.0-or-later
// Offscreen check of the in-game map's renderer (gpu/shim/map/map_vector.cpp): ImGui with its
// Vulkan backend and the map's draw callback, rendered into an image without the game or a
// window, written out as raw RGBA. Fails when nothing of the map was drawn.
//
//   bb-map-render <compiled maps> <parchment.bbra> <region> <x> <z> <zoom> <floor|-1> <out.rgba> [w h]
//
// floor -1: the area's primary floor. The image is w x h (default 1280 x 720), map centred on (x, z).
#include "map/map_vector.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

auto& Vk() {
    return VULKAN_HPP_DEFAULT_DISPATCHER;
}

void Check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(r));
}

uint32_t MemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    Vk().vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("no memory type");
}

int Run(int argc, char** argv) {
    if (argc != 9 && argc != 11) {
        std::fprintf(stderr, "usage: bb-map-render <compiled> <parchment.bbra> <region> <x> <z> <zoom> <floor|-1> <out.rgba> [w h]\n");
        return 2;
    }
    const std::string region = argv[3];
    const float x = std::strtof(argv[4], nullptr), z = std::strtof(argv[5], nullptr), zoom = std::strtof(argv[6], nullptr);
    const int want_floor = std::atoi(argv[7]);
    const uint32_t width = argc == 11 ? uint32_t(std::atoi(argv[9])) : 1280u, height = argc == 11 ? uint32_t(std::atoi(argv[10])) : 720u;
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    Vulkan::Instance instance(0, false);
    const VkDevice device = instance.GetDevice();
    const VkPhysicalDevice physical = instance.GetPhysicalDevice();
    const VkQueue queue = instance.GetGraphicsQueue();

    // Target image and readback buffer.
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kFormat;
    ici.extent = {width, height, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image{};
    Check(Vk().vkCreateImage(device, &ici, nullptr, &image), "image");
    VkMemoryRequirements req;
    Vk().vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = MemoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory image_memory{};
    Check(Vk().vkAllocateMemory(device, &mai, nullptr, &image_memory), "image memory");
    Check(Vk().vkBindImageMemory(device, image, image_memory, 0), "bind image");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = kFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{};
    Check(Vk().vkCreateImageView(device, &vci, nullptr, &view), "view");
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer readback{};
    Check(Vk().vkCreateBuffer(device, &bci, nullptr, &readback), "buffer");
    Vk().vkGetBufferMemoryRequirements(device, readback, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = MemoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory readback_memory{};
    Check(Vk().vkAllocateMemory(device, &mai, nullptr, &readback_memory), "buffer memory");
    Check(Vk().vkBindBufferMemory(device, readback, readback_memory, 0), "bind buffer");

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    VkCommandPool pool{};
    Check(Vk().vkCreateCommandPool(device, &pci, nullptr, &pool), "command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    Check(Vk().vkAllocateCommandBuffers(device, &cai, &cmd), "command buffer");

    // ImGui as the overlay sets it up (gpu/shim/bbport_overlay.cpp).
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    const VkInstance vk_instance = instance.GetInstance();
    // The instance has no window system, so its surface and swapchain functions are missing; ImGui
    // loads them for windows of its own only, which this never opens.
    if (!ImGui_ImplVulkan_LoadFunctions(
            instance.ApiVersion(),
            [](const char* name, void* user) {
                const PFN_vkVoidFunction fn = Vk().vkGetInstanceProcAddr(*static_cast<const VkInstance*>(user), name);
                if (fn || (!std::strstr(name, "Surface") && !std::strstr(name, "Swapchain"))) return fn;
                return PFN_vkVoidFunction(+[] { std::abort(); });
            },
            const_cast<VkInstance*>(&vk_instance)))
        throw std::runtime_error("ImGui Vulkan functions");
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = physical;
    info.Device = device;
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = queue;
    info.DescriptorPoolSize = 16;
    info.MinImageCount = info.ImageCount = 2;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
                                                          .colorAttachmentCount = 1,
                                                          .pColorAttachmentFormats = &kFormat};
    if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("ImGui Vulkan init");
    if (!BbMapVector::InitGpu(device, physical, kFormat)) throw std::runtime_error("map pipeline");
    BbMapVector::Configure(argv[1], argv[2]);

    // Frames until the area, its parchment and hatching are in (or 20 s).
    const auto start = std::chrono::steady_clock::now();
    int floor = -1;
    for (int frame = 0;; ++frame) {
        io.DisplaySize = ImVec2(float(width), float(height));
        io.DeltaTime = 1.0f / 60.0f;
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        auto* draw = ImGui::GetBackgroundDrawList();
        draw->AddRectFilled(ImVec2(0, 0), ImVec2(float(width), float(height)), IM_COL32(20, 18, 16, 255));
        floor = want_floor >= 0 ? want_floor : BbMapVector::Primary(region);
        if (floor >= 0)
            BbMapVector::Draw(draw, region, floor, ImVec2(width * .5f, height * .5f), x, z, zoom, 1.0f, true, height / 720.0f, nullptr);
        ImGui::Render();

        Check(Vk().vkResetCommandBuffer(cmd, 0), "reset");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(Vk().vkBeginCommandBuffer(cmd, &begin), "begin");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = vci.subresourceRange;
        Vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = view;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        Vk().vkCmdBeginRendering(cmd, &rendering);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        Vk().vkCmdEndRendering(cmd);
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        Vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        Vk().vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        Check(Vk().vkEndCommandBuffer(cmd), "end");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Check(Vk().vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit");
        Check(Vk().vkQueueWaitIdle(queue), "wait");

        const bool loaded = floor >= 0 && BbMapVector::Load(region);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        // A few frames after the load: the parchment upload and the hatch level have landed.
        static int settled = 0;
        if (loaded && ++settled > 30) break;
        if (seconds > 20) throw std::runtime_error("not loaded after 20 s: " + BbMapVector::Error());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!BbMapVector::Error().empty()) throw std::runtime_error("map error: " + BbMapVector::Error());

    void* mapped = nullptr;
    Check(Vk().vkMapMemory(device, readback_memory, 0, bytes, 0, &mapped), "map");
    std::vector<unsigned char> pixels(static_cast<unsigned char*>(mapped), static_cast<unsigned char*>(mapped) + bytes);
    Vk().vkUnmapMemory(device, readback_memory);
    std::ofstream(argv[8], std::ios::binary).write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));

    // Anything besides the background fill: the map drew. Count its distinct colours too.
    size_t drawn = 0;
    std::set<uint32_t> colours;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        uint32_t c;
        std::memcpy(&c, &pixels[i], 4);
        colours.insert(c);
        if (pixels[i] != 20 || pixels[i + 1] != 18 || pixels[i + 2] != 16) ++drawn;
    }
    std::printf("map %s floor %d: %zu of %zu pixels drawn, %zu colours (%ux%u)\n", region.c_str(), floor, drawn, pixels.size() / 4,
                colours.size(), width, height);

    BbMapVector::Shutdown();
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
    Vk().vkDestroyCommandPool(device, pool, nullptr);
    Vk().vkDestroyBuffer(device, readback, nullptr);
    Vk().vkFreeMemory(device, readback_memory, nullptr);
    Vk().vkDestroyImageView(device, view, nullptr);
    Vk().vkDestroyImage(device, image, nullptr);
    Vk().vkFreeMemory(device, image_memory, nullptr);
    return drawn > pixels.size() / 4 / 20 ? 0 : 1;  // at least 5% of the image
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bb-map-render: %s\n", e.what());
        return 1;
    }
}
