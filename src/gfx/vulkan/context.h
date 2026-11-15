#pragma once

#include <vulkan/vulkan.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/types.h"

struct SDL_Window;

namespace gfx::vulkan {

// One 3DS screen as linear RGBA8 (bytes R,G,B,A), row 0 = top.
struct ScreenImage {
    u32 width, height;
    const u32* pixels;
};

// A screen already on the GPU (renderer output, any size, GENERAL layout), screen-oriented.
struct GpuScreen {
    VkImage image;
    u32 width, height;
    VkImageView view = VK_NULL_HANDLE;  // sampled by the N64 HUD
};

constexpr u32 TOP_W = 400, BOTTOM_W = 320, SCREEN_H = 240;

// Vulkan instance/device/swapchain owner. Presents the top and bottom screens stacked,
// scaled to fit the window with their 3DS aspect ratio.
// shortcut: one frame in flight and transfer-only presentation (upload + blit);
// render passes/pipelines arrive with PICA200 draw translation.
class Context {
public:
    ~Context();
    bool init(SDL_Window* window);
    // Skips silently while the window is minimised. Returns false on a fatal device error.
    // A non-null GpuScreen replaces that screen's CPU pixels.
    bool present(const ScreenImage& top, const ScreenImage& bottom, const GpuScreen* gpu_top = nullptr,
                 const GpuScreen* gpu_bottom = nullptr);
    // Window size in pixels at the last swapchain creation (0 while minimised).
    VkExtent2D extent() const { return extent_; }
    float max_anisotropy() const { return anisotropy_; }  // 0 = unsupported

    // Presentation mode: FIFO (vsync) or the lowest-latency mode available without it.
    void set_vsync(bool on) { recreate_ |= vsync_ != on, vsync_ = on; }
    // Drawn over the composed screens inside a render pass on the swapchain image (menu).
    void set_overlay(std::function<void(VkCommandBuffer)> fn) { overlay_ = std::move(fn); }
    // N64 layout: the overlay draws the bottom screen through this descriptor set (combined
    // image sampler, binding 0), which present() points at the bottom screen image of the
    // frame; the bottom screen is then not blitted.
    void set_bottom_texture(VkDescriptorSet set) { bottom_set_ = set; }
    VkSampler sampler() const { return sampler_; }
    // Writes the next presented window image (screens + overlay) to a PPM file (validation:
    // DOT_DUMP_WINDOW). Ignored if the swapchain cannot be a transfer source.
    void dump_next_frame(std::string path) { dump_path_ = std::move(path); }
    VkImageView bottom_view() const { return screen_views_[1]; }
    VkInstance instance() const { return instance_; }
    VkRenderPass render_pass() const { return render_pass_; }
    u32 image_count() const { return u32(swapchain_images_.size()); }

    VkDevice device() const { return device_; }
    VkPhysicalDevice gpu() const { return gpu_; }
    VkQueue queue() const { return queue_; }
    u32 queue_family() const { return queue_family_; }
    // The single queue is shared with the PICA renderer (guest thread): hold this to submit.
    std::mutex& queue_lock() { return queue_lock_; }

private:
    bool create_swapchain();
    void destroy_swapchain();
    void destroy_views();
    bool create_screen_images();

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    float anisotropy_ = 0;
    std::mutex queue_lock_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkSemaphore> render_done_;  // one per swapchain image
    std::vector<VkImageView> views_;          // for the overlay render pass
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    bool vsync_ = true, recreate_ = false;  // recreate: present mode changed
    std::function<void(VkCommandBuffer)> overlay_;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore image_ready_ = VK_NULL_HANDLE;
    VkFence frame_fence_ = VK_NULL_HANDLE;

    // [0] = top, [1] = bottom; both fed from one persistently mapped staging buffer.
    VkImage screens_[2]{};
    VkImageView screen_views_[2]{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet bottom_set_ = VK_NULL_HANDLE;
    std::string dump_path_;
    bool can_dump_ = false;
    VkDeviceMemory screen_mem_[2]{};
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
    u8* staging_ptr_ = nullptr;
};

}  // namespace gfx::vulkan
