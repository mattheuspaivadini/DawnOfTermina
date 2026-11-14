#pragma once

#include <vulkan/vulkan.h>

#include <mutex>
#include <vector>

#include "core/types.h"

struct SDL_Window;

namespace gfx::vulkan {

// One 3DS screen as linear RGBA8 (bytes R,G,B,A), row 0 = top.
struct ScreenImage {
    u32 width, height;
    const u32* pixels;
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
    bool present(const ScreenImage& top, const ScreenImage& bottom);

    VkDevice device() const { return device_; }
    VkPhysicalDevice gpu() const { return gpu_; }
    VkQueue queue() const { return queue_; }
    u32 queue_family() const { return queue_family_; }
    // The single queue is shared with the PICA renderer (guest thread): hold this to submit.
    std::mutex& queue_lock() { return queue_lock_; }

private:
    bool create_swapchain();
    void destroy_swapchain();
    bool create_screen_images();

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    std::mutex queue_lock_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkSemaphore> render_done_;  // one per swapchain image

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore image_ready_ = VK_NULL_HANDLE;
    VkFence frame_fence_ = VK_NULL_HANDLE;

    // [0] = top, [1] = bottom; both fed from one persistently mapped staging buffer.
    VkImage screens_[2]{};
    VkDeviceMemory screen_mem_[2]{};
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
    u8* staging_ptr_ = nullptr;
};

}  // namespace gfx::vulkan
