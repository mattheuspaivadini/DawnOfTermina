#include "gfx/vulkan/context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gfx::vulkan {

static constexpr VkDeviceSize TOP_BYTES = VkDeviceSize(TOP_W) * SCREEN_H * 4;
static constexpr VkDeviceSize BOTTOM_BYTES = VkDeviceSize(BOTTOM_W) * SCREEN_H * 4;

static bool check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    std::fprintf(stderr, "vulkan: %s failed (VkResult %d)\n", what, int(r));
    return false;
}

static u32 find_memory(VkPhysicalDevice gpu, u32 type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if (type_bits >> i & 1 && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

static void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

Context::~Context() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        for (int i = 0; i < 2; ++i) {
            vkDestroyImage(device_, screens_[i], nullptr);
            vkFreeMemory(device_, screen_mem_[i], nullptr);
        }
        vkDestroyBuffer(device_, staging_, nullptr);
        vkFreeMemory(device_, staging_mem_, nullptr);
        vkDestroyFence(device_, frame_fence_, nullptr);
        vkDestroySemaphore(device_, image_ready_, nullptr);
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool Context::init(SDL_Window* window) {
    window_ = window;
    u32 ext_count = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!exts) {
        std::fprintf(stderr, "vulkan: %s\n", SDL_GetError());
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DawnOfTermina";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = exts;
    const char* validation = "VK_LAYER_KHRONOS_validation";
    if (std::getenv("DOT_VULKAN_VALIDATION")) {
        u32 ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> layers(ln);
        vkEnumerateInstanceLayerProperties(&ln, layers.data());
        if (std::any_of(layers.begin(), layers.end(),
                        [&](auto& l) { return std::strcmp(l.layerName, validation) == 0; })) {
            ici.enabledLayerCount = 1;
            ici.ppEnabledLayerNames = &validation;
        } else {
            std::fprintf(stderr, "vulkan: %s not installed, continuing without it\n", validation);
        }
    }
    if (!check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) return false;
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        std::fprintf(stderr, "vulkan: surface: %s\n", SDL_GetError());
        return false;
    }

    // Pick a GPU with a graphics+present queue, preferring discrete ones.
    u32 n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    int best_score = -1;
    for (VkPhysicalDevice g : gpus) {
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (u32 q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(g, &p);
            const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) best_score = score, gpu_ = g, queue_family_ = q;
            break;
        }
    }
    if (!gpu_) {
        std::fprintf(stderr, "vulkan: no GPU with graphics + present support\n");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu_, &props);
    std::printf("vulkan: using %s\n", props.deviceName);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char* dev_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &dev_ext;
    if (!check(vkCreateDevice(gpu_, &dci, nullptr, &device_), "vkCreateDevice")) return false;
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (!check(vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool")) return false;
    cai.commandPool = pool_;
    if (!check(vkAllocateCommandBuffers(device_, &cai, &cmd_), "vkAllocateCommandBuffers") ||
        !check(vkCreateSemaphore(device_, &sci, nullptr, &image_ready_), "vkCreateSemaphore") ||
        !check(vkCreateFence(device_, &fci, nullptr, &frame_fence_), "vkCreateFence"))
        return false;

    return create_screen_images() && create_swapchain();
}

bool Context::create_screen_images() {
    const u32 widths[2] = {TOP_W, BOTTOM_W};
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {widths[i], SCREEN_H, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!check(vkCreateImage(device_, &ii, nullptr, &screens_[i]), "vkCreateImage")) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, screens_[i], &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(gpu_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!check(vkAllocateMemory(device_, &mai, nullptr, &screen_mem_[i]), "vkAllocateMemory") ||
            !check(vkBindImageMemory(device_, screens_[i], screen_mem_[i], 0), "vkBindImageMemory"))
            return false;
    }

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = TOP_BYTES + BOTTOM_BYTES;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!check(vkCreateBuffer(device_, &bi, nullptr, &staging_), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, staging_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory(gpu_, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* ptr = nullptr;
    if (!check(vkAllocateMemory(device_, &mai, nullptr, &staging_mem_), "vkAllocateMemory") ||
        !check(vkBindBufferMemory(device_, staging_, staging_mem_, 0), "vkBindBufferMemory") ||
        !check(vkMapMemory(device_, staging_mem_, 0, VK_WHOLE_SIZE, 0, &ptr), "vkMapMemory"))
        return false;
    staging_ptr_ = static_cast<u8*>(ptr);
    return true;
}

bool Context::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent = {std::clamp(u32(w), caps.minImageExtent.width, caps.maxImageExtent.width),
                  std::clamp(u32(h), caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    VkSwapchainKHR old = swapchain_;
    for (VkSemaphore s : render_done_) vkDestroySemaphore(device_, s, nullptr);
    render_done_.clear();
    swapchain_images_.clear();
    swapchain_ = VK_NULL_HANDLE;
    extent_ = extent;
    if (!extent.width || !extent.height) {  // minimised
        vkDestroySwapchainKHR(device_, old, nullptr);
        return true;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        std::fprintf(stderr, "vulkan: swapchain images cannot be transfer destinations\n");
        return false;
    }

    u32 n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    // 3DS framebuffers are already gamma-encoded: want a UNORM format in plain sRGB space
    // (an *_SRGB format or a linear colour space would apply gamma a second time).
    VkSurfaceFormatKHR fmt = formats.at(0);
    for (auto f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = f;
            break;
        }
    swapchain_format_ = fmt.format;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount)
                                          : caps.minImageCount + 1;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available; vsync
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    const bool ok = check(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    vkDestroySwapchainKHR(device_, old, nullptr);
    if (!ok) return false;

    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapchain_images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapchain_images_.data());
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    render_done_.resize(n);
    for (auto& s : render_done_)
        if (!check(vkCreateSemaphore(device_, &sci, nullptr, &s), "vkCreateSemaphore")) return false;
    return true;
}

void Context::destroy_swapchain() {
    for (VkSemaphore s : render_done_) vkDestroySemaphore(device_, s, nullptr);
    render_done_.clear();
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Context::present(const ScreenImage& top, const ScreenImage& bottom) {
    if (top.width != TOP_W || bottom.width != BOTTOM_W || top.height != SCREEN_H ||
        bottom.height != SCREEN_H) {
        std::fprintf(stderr, "vulkan: unexpected screen size\n");
        return false;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (u32(w) != extent_.width || u32(h) != extent_.height || !swapchain_) {
        vkDeviceWaitIdle(device_);
        if (!create_swapchain()) return false;
        if (!swapchain_) return true;  // minimised
    }

    vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, UINT64_MAX);
    u32 idx = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_ready_, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        extent_ = {};  // force recreation next frame
        return true;
    }
    if (r != VK_SUBOPTIMAL_KHR && !check(r, "vkAcquireNextImageKHR")) return false;
    vkResetFences(device_, 1, &frame_fence_);

    std::memcpy(staging_ptr_, top.pixels, TOP_BYTES);
    std::memcpy(staging_ptr_ + TOP_BYTES, bottom.pixels, BOTTOM_BYTES);

    VkCommandBuffer cmd = cmd_;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    const u32 widths[2] = {TOP_W, BOTTOM_W};
    const VkDeviceSize offsets[2] = {0, TOP_BYTES};
    for (int i = 0; i < 2; ++i) {
        image_barrier(cmd, screens_[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offsets[i];
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {widths[i], SCREEN_H, 1};
        vkCmdCopyBufferToImage(cmd, staging_, screens_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        image_barrier(cmd, screens_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT);
    }

    VkImage target = swapchain_images_[idx];
    image_barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    image_barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    // 400x480 virtual canvas: top screen above, bottom screen centred below.
    const float s = std::min(extent_.width / 400.0f, extent_.height / 480.0f);
    const s32 ox = s32((extent_.width - 400 * s) / 2), oy = s32((extent_.height - 480 * s) / 2);
    const s32 dst[2][4] = {
        {ox, oy, ox + s32(400 * s), oy + s32(240 * s)},
        {ox + s32(40 * s), oy + s32(240 * s), ox + s32(360 * s), oy + s32(480 * s)},
    };
    for (int i = 0; i < 2; ++i) {
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {s32(widths[i]), s32(SCREEN_H), 1};
        blit.dstOffsets[0] = {dst[i][0], dst[i][1], 0};
        blit.dstOffsets[1] = {dst[i][2], dst[i][3], 1};
        vkCmdBlitImage(cmd, screens_[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
    VkImageMemoryBarrier to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = target;
    to_present.subresourceRange = range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_present);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &image_ready_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_done_[idx];
    if (!check(vkQueueSubmit(queue_, 1, &si, frame_fence_), "vkQueueSubmit")) return false;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_done_[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) extent_ = {};
    else if (!check(r, "vkQueuePresentKHR")) return false;
    return true;
}

}  // namespace gfx::vulkan
