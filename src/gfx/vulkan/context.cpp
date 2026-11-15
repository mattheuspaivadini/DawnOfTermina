#include "gfx/vulkan/context.h"

#include "gfx/layout.h"

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
    // Fragment stage too: the N64 HUD samples the bottom screen in the overlay pass.
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

Context::~Context() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        vkDestroySampler(device_, sampler_, nullptr);
        for (int i = 0; i < 2; ++i) {
            vkDestroyImageView(device_, screen_views_[i], nullptr);
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
    VkPhysicalDeviceFeatures supported{}, features{};
    vkGetPhysicalDeviceFeatures(gpu_, &supported);
    features.samplerAnisotropy = supported.samplerAnisotropy;  // used when rendering above 1x
    anisotropy_ = supported.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 0.0f;
    dci.pEnabledFeatures = &features;
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
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!check(vkCreateImage(device_, &ii, nullptr, &screens_[i]), "vkCreateImage")) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, screens_[i], &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(gpu_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!check(vkAllocateMemory(device_, &mai, nullptr, &screen_mem_[i]), "vkAllocateMemory") ||
            !check(vkBindImageMemory(device_, screens_[i], screen_mem_[i], 0), "vkBindImageMemory"))
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = screens_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!check(vkCreateImageView(device_, &vi, nullptr, &screen_views_[i]), "vkCreateImageView")) return false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!check(vkCreateSampler(device_, &si, nullptr, &sampler_), "vkCreateSampler")) return false;

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
    destroy_views();
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
    if (!render_pass_) {  // overlay pass: keeps the composed screens, leaves the image ready to present
        VkAttachmentDescription at{};
        at.format = fmt.format;
        at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1, rp.pAttachments = &at;
        rp.subpassCount = 1, rp.pSubpasses = &sub;
        rp.dependencyCount = 1, rp.pDependencies = &dep;
        if (!check(vkCreateRenderPass(device_, &rp, nullptr, &render_pass_), "vkCreateRenderPass")) return false;
    }

    // Without vsync: mailbox (no tearing) if available, else immediate.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // always available
    if (!vsync_) {
        u32 pn = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &pn, nullptr);
        std::vector<VkPresentModeKHR> modes(pn);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &pn, modes.data());
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (mode == VK_PRESENT_MODE_FIFO_KHR && std::find(modes.begin(), modes.end(), m) != modes.end()) mode = m;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount)
                                          : caps.minImageCount + 1;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    can_dump_ = caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (can_dump_) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
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
    for (VkImage img : swapchain_images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (!check(vkCreateImageView(device_, &vi, nullptr, &view), "vkCreateImageView")) return false;
        views_.push_back(view);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = render_pass_;
        fi.attachmentCount = 1, fi.pAttachments = &view;
        fi.width = extent.width, fi.height = extent.height, fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (!check(vkCreateFramebuffer(device_, &fi, nullptr, &fb), "vkCreateFramebuffer")) return false;
        framebuffers_.push_back(fb);
    }
    return true;
}

void Context::destroy_views() {
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(device_, f, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    framebuffers_.clear(), views_.clear();
}

void Context::destroy_swapchain() {
    for (VkSemaphore s : render_done_) vkDestroySemaphore(device_, s, nullptr);
    render_done_.clear();
    destroy_views();
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Context::present(const ScreenImage& top, const ScreenImage& bottom, const GpuScreen* gpu_top,
                      const GpuScreen* gpu_bottom) {
    const GpuScreen* gpu_screens[2] = {gpu_top, gpu_bottom};
    if (top.width != TOP_W || bottom.width != BOTTOM_W || top.height != SCREEN_H ||
        bottom.height != SCREEN_H) {
        std::fprintf(stderr, "vulkan: unexpected screen size\n");
        return false;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (u32(w) != extent_.width || u32(h) != extent_.height || !swapchain_ || recreate_) {
        recreate_ = false;
        {
            std::lock_guard qlk(queue_lock_);
            vkQueueWaitIdle(queue_);
        }
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
    // N64 HUD: the overlay samples this frame's bottom screen (the fence wait above means the
    // previous frame no longer reads the descriptor).
    const bool hud = g_n64 && bottom_set_ && overlay_;
    if (hud) {
        const VkDescriptorImageInfo info{sampler_, gpu_bottom && gpu_bottom->view ? gpu_bottom->view : screen_views_[1],
                                         VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = bottom_set_;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    if (!gpu_top) std::memcpy(staging_ptr_, top.pixels, TOP_BYTES);
    if (!gpu_bottom) std::memcpy(staging_ptr_ + TOP_BYTES, bottom.pixels, BOTTOM_BYTES);

    VkCommandBuffer cmd = cmd_;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    const u32 widths[2] = {TOP_W, BOTTOM_W};
    const VkDeviceSize offsets[2] = {0, TOP_BYTES};
    for (int i = 0; i < 2; ++i) {
        if (gpu_screens[i]) {  // rendered by the PICA renderer, kept in GENERAL layout
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0,
                                 nullptr, 0, nullptr);
            continue;
        }
        image_barrier(cmd, screens_[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offsets[i];
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {widths[i], SCREEN_H, 1};
        vkCmdCopyBufferToImage(cmd, staging_, screens_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        image_barrier(cmd, screens_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    }

    VkImage target = swapchain_images_[idx];
    image_barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    image_barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    // A widened top screen image (renderer widescreen) switches to the widescreen layout.
    g_top_width = gpu_top ? 240.0f * float(gpu_top->width) / float(gpu_top->height) : 400.0f;
    Rect rects[2];
    screen_layout(float(extent_.width), float(extent_.height), rects[0], rects[1]);
    s32 dst[2][4];
    for (int i = 0; i < 2; ++i)
        dst[i][0] = s32(rects[i].x), dst[i][1] = s32(rects[i].y), dst[i][2] = s32(rects[i].x + rects[i].w),
        dst[i][3] = s32(rects[i].y + rects[i].h);
    for (int i = 0; i < 2; ++i) {
        if (dst[i][2] <= dst[i][0] || dst[i][3] <= dst[i][1]) continue;  // hidden overlay
        if (i == 1 && hud) continue;  // drawn by the overlay
        const GpuScreen* g = gpu_screens[i];
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = g ? VkOffset3D{s32(g->width), s32(g->height), 1} : VkOffset3D{s32(widths[i]), s32(SCREEN_H), 1};
        blit.dstOffsets[0] = {dst[i][0], dst[i][1], 0};
        blit.dstOffsets[1] = {dst[i][2], dst[i][3], 1};
        vkCmdBlitImage(cmd, g ? g->image : screens_[i], VK_IMAGE_LAYOUT_GENERAL,
                       target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, g ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    if (overlay_) {  // the render pass also moves the image to PRESENT_SRC
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = render_pass_;
        rbi.framebuffer = framebuffers_[idx];
        rbi.renderArea = {{0, 0}, extent_};
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        overlay_(cmd);
        vkCmdEndRenderPass(cmd);
    }
    VkImageMemoryBarrier to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = target;
    to_present.subresourceRange = range;
    if (!overlay_)
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_present);
    // DOT_DUMP_WINDOW: copy the finished image (PRESENT_SRC) to a host buffer.
    VkBuffer dump_buf = VK_NULL_HANDLE;
    VkDeviceMemory dump_mem = VK_NULL_HANDLE;
    if (!dump_path_.empty() && can_dump_) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize(extent_.width) * extent_.height * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device_, &bi, nullptr, &dump_buf);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, dump_buf, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(gpu_, req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &mai, nullptr, &dump_mem);
        vkBindBufferMemory(device_, dump_buf, dump_mem, 0);
        VkImageMemoryBarrier b = to_present;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump_buf, 1, &copy);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &image_ready_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_done_[idx];
    std::lock_guard qlk(queue_lock_);
    if (!check(vkQueueSubmit(queue_, 1, &si, frame_fence_), "vkQueueSubmit")) return false;
    if (dump_buf) {
        vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, UINT64_MAX);
        void* p = nullptr;
        vkMapMemory(device_, dump_mem, 0, VK_WHOLE_SIZE, 0, &p);
        const bool bgr = swapchain_format_ == VK_FORMAT_B8G8R8A8_UNORM;
        if (std::FILE* f = std::fopen(dump_path_.c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", extent_.width, extent_.height);
            const u8* px = static_cast<const u8*>(p);
            for (size_t i = 0; i < size_t(extent_.width) * extent_.height; ++i, px += 4) {
                const u8 rgb[3] = {px[bgr ? 2 : 0], px[1], px[bgr ? 0 : 2]};
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
        vkDestroyBuffer(device_, dump_buf, nullptr);
        vkFreeMemory(device_, dump_mem, nullptr);
        dump_path_.clear();
    }

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
