// DawnOfTermina — native static recompilation of Majora's Mask 3D.
// Copyright (C) 2026 DawnOfTermina contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "gfx/vulkan/renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "gfx/pica/pixel.h"
#include "gfx/pica/shader_gen.h"
#include "gfx/pica/shader_interp.h"
#include "gfx/pica/texture.h"
#include "gfx/pica/vertex_loader.h"
#include "gfx/vulkan/context.h"
#include "gfx/vulkan/spirv.h"

namespace gfx::vulkan {

namespace {

using core::Memory;
using pica::f24_to_f32;

// PICA registers used here (word indices).
constexpr u32 CULL = 0x040, VIEWPORT_W = 0x041, VIEWPORT_H = 0x043, DEPTH_SCALE = 0x04D, DEPTH_OFFSET = 0x04E;
constexpr u32 OUTMAP = 0x050, SCISSOR_MODE = 0x065, SCISSOR_MIN = 0x066, SCISSOR_MAX = 0x067, VIEWPORT_XY = 0x068;
constexpr u32 TEX_CONFIG = 0x080, COLOR_OP = 0x100, BLEND_FUNC = 0x101, BLEND_COLOR = 0x103, STENCIL_TEST = 0x105,
              STENCIL_OP = 0x106;
constexpr u32 DEPTH_COLOR_MASK = 0x107, COLOR_WRITE_ENABLE = 0x113, DEPTH_WRITE_ENABLE = 0x115;
constexpr u32 DEPTH_FORMAT = 0x116, COLOR_FORMAT = 0x117, DEPTH_ADDR = 0x11C, COLOR_ADDR = 0x11D, FB_DIM = 0x11E;
constexpr u32 PRIMITIVE_CONFIG = 0x25E, VS_ENTRY = 0x2BA;
// Texture units: dims, param, address, type.
constexpr u32 TEX_REGS[3][4] = {{0x082, 0x083, 0x085, 0x08E}, {0x092, 0x093, 0x095, 0x096}, {0x09A, 0x09B, 0x09D, 0x09E}};

constexpr u32 UBO_SIZE = 96 * 16 + 4 * 16 + 16 + 16;
constexpr u32 LIGHT_REGS = 0x140, LIGHT_REG_WORDS = 0x90;  // fragment lighting buffer, see shader_gen.h
constexpr u32 LIGHT_BUF_SIZE = (LIGHT_REG_WORDS + 24 * 256 + 128) * 4;  // + fog LUT

// Only D24S8 depth buffers have a stencil channel.
bool has_stencil(const std::array<u32, pica::reg::COUNT>& regs) { return (regs[DEPTH_FORMAT] & 3) == 3; }

VkStencilOp stencil_op(u32 op) {
    static constexpr VkStencilOp map[] = {
        VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
        VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP,
        VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return map[op & 7];
}

// A vertex after the (CPU-run) PICA vertex shader, packed by output semantic.
struct ShadedVertex {
    float pos[4], color[4], uv01[4] /* t0.xy t1.xy */, uv2w[4] /* t2.xy t0.w */, quat[4], view[4];
};

// Pass-through vertex shader: PICA clip space -> Vulkan, fixed interface of shader_gen.h.
constexpr const char* PASS_VS = R"(#version 450
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec4 in_uv01;
layout(location = 3) in vec4 in_uv2w;
layout(location = 4) in vec4 in_quat;
layout(location = 5) in vec4 in_view;
layout(std140, set = 0, binding = 0) uniform PicaVsUniforms { vec4 c[96]; ivec4 i[4]; vec4 depth; uint b; } u;
layout(location = 0) out vec4 primary_color;
layout(location = 1) out vec2 texcoord0;
layout(location = 2) out vec2 texcoord1;
layout(location = 3) out vec2 texcoord2;
layout(location = 4) out float texcoord0_w;
layout(location = 5) out vec4 normquat;
layout(location = 6) out vec3 view;
void main() {
    // PICA window depth = z/w * scale + offset; pre-multiplied by w for Vulkan. depth.z: widescreen.
    gl_Position = vec4(in_pos.x, in_pos.y * u.depth.z, in_pos.z * u.depth.x + in_pos.w * u.depth.y, in_pos.w);
    primary_color = min(abs(in_color), vec4(1.0));
    texcoord0 = in_uv01.xy;
    texcoord1 = in_uv01.zw;
    texcoord2 = in_uv2w.xy;
    texcoord0_w = in_uv2w.z;
    normquat = in_quat;
    view = in_view.xyz;
}
)";

u64 fnv(const void* data, size_t n, u64 h = 1469598103934665603ull) {
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Sampled 64-bit hash of guest memory: one 8-byte word every 256 bytes plus the tail. Surfaces
// are hashed on every bind to catch CPU writes; sampling keeps that cheap.
// shortcut: CPU writes that touch none of the sampled words go unnoticed (whole-buffer writes,
// fills and videos always do); hash every word if that ever shows up.
u64 hash_ram(const u8* p, size_t n) {
    u64 h = n;
    size_t i = 0;
    for (; i + 8 <= n; i += 256) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Full 64-bit hash, 8 bytes per step (texture contents: every byte matters).
u64 hash_words(const u8* p, size_t n) {
    u64 h = n, a = 0x243F6A8885A308D3ull;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {  // two independent lanes
        u64 w0, w1;
        std::memcpy(&w0, p + i, 8), std::memcpy(&w1, p + i + 8, 8);
        h = (h ^ w0) * 0x9E3779B97F4A7C15ull;
        a = (a ^ w1) * 0xC2B2AE3D27D4EB4Full;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h ^ (a + (h >> 31));
}

// DisplayTransfer on the GPU: colour surface -> screen-oriented image (framebuffer rows are
// screen columns, bottom to top). a = {flip, x downscale shift, y downscale shift, source height},
// b.x = source rows per output column (> 1 when a widened surface feeds a normal-width screen).
constexpr const char* TRANSFER_CS = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { ivec4 a; vec4 b; } p;
void main() {
    const ivec2 o = ivec2(gl_GlobalInvocationID.xy), size = imageSize(dst);
    if (o.x >= size.x || o.y >= size.y) return;
    const int x = p.a.x != 0 ? size.x - 1 - o.x : o.x;  // output framebuffer row = screen column
    const int row = int((float(x) + 0.5) * p.b.x);
    const int col = (size.y - 1 - o.y) << p.a.y;
    const int mem_row = row << p.a.z;                      // colour buffers are bottom-up
    const int nx = 1 << p.a.y, ny = 1 << p.a.z;
    vec4 c = vec4(0.0);
    for (int dy = 0; dy < ny; ++dy)
        for (int dx = 0; dx < nx; ++dx) c += texelFetch(src, ivec2(col + dx, p.a.w - 1 - (mem_row + dy)), 0);
    imageStore(dst, o, vec4(c.rgb / float(nx * ny), 1.0));
}
)";

bool check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    std::fprintf(stderr, "renderer: %s failed (VkResult %d)\n", what, int(r));
    return false;
}

VkBlendFactor blend_factor(u32 f) {
    static constexpr VkBlendFactor map[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return f < 15 ? map[f] : VK_BLEND_FACTOR_ONE;
}

VkBlendOp blend_op(u32 e) {
    static constexpr VkBlendOp map[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                        VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return e < 5 ? map[e] : VK_BLEND_OP_ADD;
}

VkCompareOp compare_op(u32 f) {
    static constexpr VkCompareOp map[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL,
                                          VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
                                          VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
    return map[f & 7];
}

VkSamplerAddressMode wrap_mode(u32 w) {
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 2: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

}  // namespace

struct Renderer::Impl {
    Context& ctx;
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;

    struct Buffer {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        u8* ptr = nullptr;
        VkDeviceSize size = 0;
    };
    struct Image {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    // A guest colour buffer on the GPU, `scale` times its guest size. All cached images stay
    // in GENERAL layout so they can be attachments, sampled, blitted and cleared without
    // tracking layouts; barriers only order memory.
    struct Surface {
        u32 va = 0, width = 0, height = 0, format = 0;  // guest size and PICA colour format
        u32 image_h = 0;  // image rows: height * scale, times the widescreen factor for the top screen
        Image color;
        float sy() const { return float(image_h) / float(height); }  // image rows per guest row
        bool gpu_newer = false;  // drawn to since guest memory was last in sync
        u64 ram_hash = 0;        // guest memory when it was last in sync
    };
    struct Depth {
        u32 width = 0, height = 0, image_h = 0;
        Image image;
    };
    struct Display {  // DisplayTransfer output, screen-oriented, scaled
        Image image;
        Image interp;              // frame interpolation: the in-between frame shown before `image`
        bool interp_fresh = false;  // `interp` goes with the current `image`
        std::chrono::steady_clock::time_point ready;  // when `image` and `interp` were both done
        std::chrono::steady_clock::duration period{std::chrono::microseconds(33'430)};  // between real frames
        u32 width = 0, height = 0, bytes = 0;
        u64 ram_hash = 0;  // guest memory at the output when the transfer ran
    };
    struct Texture {
        Image image;
        u32 width = 0, height = 0;
        u64 hash = 0;
        bool ready = false;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layouts[2]{};
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    Buffer vbuf, ubuf, lbuf, xfer;
    // Draws are recorded into `cmd` and submitted as one batch by flush() (or when a buffer
    // or the descriptor pool fills up); each buffer is sub-allocated linearly per batch.
    bool recording = false;
    u32 batch_draws = 0;
    VkDeviceSize vb_used = 0, ub_used = 0, lb_used = 0, x_used = 0, lb_last = 0;
    std::vector<u32> last_light;  // lighting data at lb_last, to skip re-uploading it
    u32 scale = 1;
    float anisotropy = 0;
    bool always_readback = std::getenv("DOT_READBACK") != nullptr;
    std::map<u32, Surface> surfaces;              // by colour buffer VA
    std::map<u32, Depth> depths;                  // by depth buffer VA
    std::map<u64, VkFramebuffer> framebuffers;    // by colour VA << 32 | depth VA
    std::map<u32, Display> displays;              // by output VA
    std::map<u32, Image> scratch;                 // guest-size staging images, by width << 16 | height
    Surface* cur = nullptr;                       // current render target

    // Frame interpolation (DOT_FPS=60): the game simulates at 30 Hz (one step per 2 VBlanks,
    // fixed-step logic). Every GPU operation of a top-screen frame is recorded; when the frame
    // ends (its DisplayTransfer to the 400-row framebuffer) it is replayed with the vertex
    // shaders' float uniforms (camera, model and bone matrices) averaged with the matching
    // draw of the previous frame, into an extra screen image shown one VBlank before the real
    // frame. Guest memory still holds everything the frame used at that point (the real GPU
    // reads it asynchronously, so the game cannot have reused it yet).
    struct Op {
        enum Kind : u8 { Draw, Fill, Transfer } kind;
        bool indexed = false, gpu = true;  // gpu: the original ran on the GPU path (else skipped)
        std::array<u32, pica::reg::COUNT> regs;
        std::shared_ptr<const pica::ShaderState> vs;
        u32 a[6] = {};
        u64 key = 0;  // draws: what it draws (program, buffers, texture) + ordinal in the frame
    };
    bool interpolate = false, replaying = false;
    std::vector<Op> frame_ops, prev_ops;
    std::map<u64, u32> key_counts;
    std::shared_ptr<pica::ShaderState> last_vs;
    u64 interp_frames = 0, interp_skipped = 0;
    // Widescreen: the colour buffers that feed the top screen get W times more rows (rows are
    // the screen's horizontal axis). DOT_WIDESCREEN=<w>:<h> or "window" (follows the window).
    float wide_aspect = 0;  // top screen aspect; 0 = off, < 0 = window
    std::set<u32> wide_vas;
    u32 cur_depth = 0;
    VkDescriptorSetLayout xfer_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout xfer_layout = VK_NULL_HANDLE;
    VkPipeline xfer_pipe = VK_NULL_HANDLE;
    Texture dummy;
    std::map<u64, Texture> textures;  // by (va, size, format)
    std::map<u32, VkSampler> samplers;
    std::map<u64, VkShaderModule> modules;
    VkShaderModule pass_vs = VK_NULL_HANDLE;
    std::map<u64, VkShaderModule> vertex_shaders;  // by program + bool/int uniforms; null = CPU fallback
    std::map<u64, VkPipeline> pipelines;
    struct DepthClear {
        float depth;
        u32 stencil;
    };
    std::map<u32, DepthClear> depth_clears;  // last MemoryFill value per address, normalised
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    static constexpr VkImageAspectFlags DS_ASPECT = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    std::mutex lock;  // draw/flush may come from different guest threads
    Stats stats{};

    // On-disk caches so a scene compiles its shaders once, not once per run: SPIR-V by GLSL
    // hash (skips glslang) and the driver's VkPipelineCache. Saved at most every 10 s while
    // new entries appear (the process is often killed rather than closed) and on shutdown.
    // shortcut: entries are never evicted; delete the cache directory if it grows too big.
    std::filesystem::path cache_dir;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    std::map<u64, std::vector<u32>> spirv_cache;
    bool cache_dirty = false;
    std::chrono::steady_clock::time_point cache_saved = std::chrono::steady_clock::now();
    static constexpr u32 SPIRV_CACHE_MAGIC = 0x31565053;  // "SPV1"

    explicit Impl(Context& c) : ctx(c) {}

    u32 memory_type(u32 bits, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
        for (u32 i = 0; i < mp.memoryTypeCount; ++i)
            if (bits >> i & 1 && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
        return 0;
    }

    void ensure(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
        if (b.size >= size) return;
        if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr), vkFreeMemory(dev, b.mem, nullptr);
        size = std::max<VkDeviceSize>(size, 64 * 1024);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        vkCreateBuffer(dev, &bi, nullptr, &b.buf);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, b.buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(dev, &ai, nullptr, &b.mem);
        vkBindBufferMemory(dev, b.buf, b.mem, 0);
        void* p;
        vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p);
        b.ptr = static_cast<u8*>(p), b.size = size;
    }

    Image make_image(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        Image im;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {w, h, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        vkCreateImage(dev, &ii, nullptr, &im.img);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, im.img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &ai, nullptr, &im.mem);
        vkBindImageMemory(dev, im.img, im.mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCreateImageView(dev, &vi, nullptr, &im.view);
        return im;
    }

    void destroy(Image& im) {
        if (!im.img) return;
        vkDestroyImageView(dev, im.view, nullptr);
        vkDestroyImage(dev, im.img, nullptr);
        vkFreeMemory(dev, im.mem, nullptr);
        im = {};
    }

    void barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from, b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }

    static constexpr u32 MAX_BATCH_DRAWS = 1024;

    static VkDeviceSize align(VkDeviceSize v) { return (v + 255) & ~VkDeviceSize(255); }  // >= any offset alignment

    // Takes `size` bytes from a buffer for the current batch (callers checked it fits).
    static VkDeviceSize take(VkDeviceSize& used, VkDeviceSize size) {
        const VkDeviceSize off = used;
        used += align(size);
        return off;
    }

    // Opens the batch's command buffer if needed.
    void begin() {
        if (recording) return;
        recording = true;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
    }

    // Submits the open batch (if any), waits for it and recycles the per-batch resources.
    void submit_and_wait() {
        if (!recording) return;
        recording = false;
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkResetFences(dev, 1, &fence);
        {
            std::lock_guard qlk(ctx.queue_lock());
            check(vkQueueSubmit(ctx.queue(), 1, &si, fence), "vkQueueSubmit");
        }
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetDescriptorPool(dev, desc_pool, 0);
        batch_draws = 0, vb_used = ub_used = lb_used = x_used = 0;
        last_light.clear();
    }

    static std::vector<u8> read_file(const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }

    void load_caches() {
        const auto pc = read_file(cache_dir / "pipeline_cache.bin");
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = pc.size(), ci.pInitialData = pc.data();  // the driver rejects foreign data itself
        if (vkCreatePipelineCache(dev, &ci, nullptr, &pipeline_cache) != VK_SUCCESS) {
            ci.initialDataSize = 0;
            vkCreatePipelineCache(dev, &ci, nullptr, &pipeline_cache);
        }
        // spirv_cache.bin: magic, then { u64 key; u32 words; u32 code[words] }...
        const auto sc = read_file(cache_dir / "spirv_cache.bin");
        size_t at = 4;
        u32 magic = 0;
        if (sc.size() >= 4) std::memcpy(&magic, sc.data(), 4);
        while (magic == SPIRV_CACHE_MAGIC && at + 12 <= sc.size()) {
            u64 key;
            u32 words;
            std::memcpy(&key, &sc[at], 8), std::memcpy(&words, &sc[at + 8], 4);
            at += 12;
            if (words > (sc.size() - at) / 4) break;  // truncated file: keep what was complete
            auto& code = spirv_cache[key];
            code.resize(words);
            std::memcpy(code.data(), &sc[at], size_t(words) * 4);
            at += size_t(words) * 4;
        }
    }

    // Writes to a temporary file first so a kill mid-save cannot leave a corrupt cache.
    static void write_file(const std::filesystem::path& p, const std::vector<u8>& data) {
        const auto tmp = std::filesystem::path(p) += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            if (!f) return;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, p, ec);
    }

    void save_caches() {
        if (!cache_dirty || cache_dir.empty()) return;
        cache_dirty = false;
        cache_saved = std::chrono::steady_clock::now();
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
        size_t size = 0;
        vkGetPipelineCacheData(dev, pipeline_cache, &size, nullptr);
        std::vector<u8> data(size);
        if (vkGetPipelineCacheData(dev, pipeline_cache, &size, data.data()) == VK_SUCCESS)
            write_file(cache_dir / "pipeline_cache.bin", data);
        data.assign(4, 0);
        std::memcpy(data.data(), &SPIRV_CACHE_MAGIC, 4);
        for (const auto& [key, code] : spirv_cache) {
            const u32 words = u32(code.size());
            const size_t at = data.size();
            data.resize(at + 12 + code.size() * 4);
            std::memcpy(&data[at], &key, 8), std::memcpy(&data[at + 8], &words, 4);
            std::memcpy(&data[at + 12], code.data(), code.size() * 4);
        }
        write_file(cache_dir / "spirv_cache.bin", data);
    }

    bool init(const std::filesystem::path& dir) {
        dev = ctx.device(), gpu = ctx.gpu();
        cache_dir = dir;
        load_caches();
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = ctx.queue_family();
        if (!check(vkCreateCommandPool(dev, &pci, nullptr, &pool), "vkCreateCommandPool")) return false;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &cai, &cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev, &fci, nullptr, &fence);

        // Depth + stencil: D32S8 where available (float depth keeps precision), else D24S8.
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu, depth_format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            depth_format = VK_FORMAT_D24_UNORM_S8_UINT;

        // Render pass: the colour/depth/stencil targets keep their contents between draws.
        VkAttachmentDescription att[2]{};
        att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        att[1].format = depth_format;
        for (int i = 0; i < 2; ++i) {
            att[i].samples = VK_SAMPLE_COUNT_1_BIT;
            att[i].loadOp = att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att[i].storeOp = att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[i].initialLayout = att[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_GENERAL};
        VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        sub.pDepthStencilAttachment = &dref;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 2;
        rpi.pAttachments = att;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sub;
        if (!check(vkCreateRenderPass(dev, &rpi, nullptr, &render_pass), "vkCreateRenderPass")) return false;

        const VkDescriptorSetLayoutBinding bufs[2] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutBinding tex[3];
        for (u32 i = 0; i < 3; ++i)
            tex[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 2, li.pBindings = bufs;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &set_layouts[0]);
        li.bindingCount = 3, li.pBindings = tex;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &set_layouts[1]);
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = set_layouts;
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, pica::FRAGMENT_PUSH_WORDS * 4};
        pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push;
        vkCreatePipelineLayout(dev, &pli, nullptr, &pipeline_layout);
        VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_BATCH_DRAWS},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * MAX_BATCH_DRAWS},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_BATCH_DRAWS},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_BATCH_DRAWS}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 3 * MAX_BATCH_DRAWS;
        dpi.poolSizeCount = 4;
        dpi.pPoolSizes = sizes;
        vkCreateDescriptorPool(dev, &dpi, nullptr, &desc_pool);

        ensure(vbuf, 8 << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        ensure(ubuf, MAX_BATCH_DRAWS * align(UBO_SIZE), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        ensure(lbuf, 64 * align(LIGHT_BUF_SIZE), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        ensure(xfer, 16 << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        // DisplayTransfer compute pipeline.
        const VkDescriptorSetLayoutBinding xb[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        li.bindingCount = 2, li.pBindings = xb;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &xfer_set_layout);
        const VkPushConstantRange xpush{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        pli.setLayoutCount = 1, pli.pSetLayouts = &xfer_set_layout;
        pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &xpush;
        vkCreatePipelineLayout(dev, &pli, nullptr, &xfer_layout);
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                     module(TRANSFER_CS, ShaderStage::Compute), "main", nullptr};
        cpi.layout = xfer_layout;
        if (!cpi.stage.module ||
            !check(vkCreateComputePipelines(dev, pipeline_cache, 1, &cpi, nullptr, &xfer_pipe), "transfer pipeline"))
            return false;

        // Resolution scale: DOT_RES_SCALE=<1..8> or "native" (the window's size at startup).
        if (const char* e = std::getenv("DOT_WIDESCREEN")) {
            float aw = 0, ah = 0;
            if (std::string(e) == "window") wide_aspect = -1;
            else if (std::sscanf(e, "%f:%f", &aw, &ah) == 2 && aw > 0 && ah > 0) wide_aspect = aw / ah;
        }
        if (const char* e = std::getenv("DOT_RES_SCALE")) {
            if (std::string(e) == "native") {  // the top screen fills the window height in widescreen
                const VkExtent2D ext = ctx.extent();
                scale = wide_aspect != 0 ? std::max(1u, ext.height / 240)
                                         : std::max(1u, std::min(ext.width / 400, ext.height / 480));
            } else {
                scale = u32(std::clamp(std::atoi(e), 1, 8));
            }
            scale = std::min(scale, 8u);
        }
        anisotropy = scale > 1 ? std::min(16.0f, ctx.max_anisotropy()) : 0.0f;
        if (const char* e = std::getenv("DOT_FPS")) interpolate = std::atoi(e) >= 60;
        std::printf("renderer: resolution scale %ux%s\n", scale, anisotropy > 1 ? ", anisotropic filtering" : "");

        // 1x1 white texture for disabled units.
        dummy.image = make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        dummy.width = dummy.height = 1;
        const u32 white = 0xFFFFFFFF;
        std::memcpy(xfer.ptr, &white, 4);
        begin();
        upload_image(dummy.image.img, 1, 1, 0);
        submit_and_wait();
        return true;
    }

    // Records a buffer->image copy from xfer at `offset`, leaving the image shader-readable.
    void upload_image(VkImage img, u32 w, u32 h, VkDeviceSize offset) {
        barrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, xfer.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    VkShaderModule module(const std::string& glsl, ShaderStage stage) {
        const u64 key = fnv(glsl.data(), glsl.size(), u64(stage));
        auto it = modules.find(key);
        if (it != modules.end()) return it->second;
        std::string log;
        auto& spv = spirv_cache[key];
        if (spv.empty()) {
            spv = compile_glsl(glsl, stage, &log);
            cache_dirty = !spv.empty();
            ++stats.shaders;
            if (std::getenv("DOT_TRACE_SHADERS") && stage == ShaderStage::Fragment)
                std::fprintf(stderr, "---- new fragment shader ----\n%s\n", glsl.c_str());
        }
        VkShaderModule m = VK_NULL_HANDLE;
        if (spv.empty()) {
            std::fprintf(stderr, "renderer: shader compile failed:\n%s\n", log.c_str());
        } else {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = spv.size() * 4;
            mi.pCode = spv.data();
            vkCreateShaderModule(dev, &mi, nullptr, &m);
        }
        modules[key] = m;
        return m;
    }

    VkSampler sampler(u32 param) {
        const u32 key = param & 0x770E;
        auto it = samplers.find(key);
        if (it != samplers.end()) return it->second;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = param >> 1 & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = param >> 2 & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.addressModeU = wrap_mode(param >> 12 & 7);
        si.addressModeV = wrap_mode(param >> 8 & 7);
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        si.maxLod = 0;
        si.anisotropyEnable = anisotropy > 1 && si.minFilter == VK_FILTER_LINEAR;
        si.maxAnisotropy = si.anisotropyEnable ? anisotropy : 1.0f;
        VkSampler s;
        vkCreateSampler(dev, &si, nullptr, &s);
        samplers[key] = s;
        return s;
    }

    // Guest colour buffer (tiled, guest format) <-> Vulkan image rows (window y). PICA lays
    // colour buffers out bottom-up: memory row r holds window row H - 1 - r.
    void read_guest_color(const Memory& mem, const Surface& sf, u8* out) {
        const u32 bpp = pica::pixel::bytes_per_pixel(sf.format);
        const u8* src = mem.host(sf.va);
        for (u32 y = 0; y < sf.height; ++y)
            for (u32 x = 0; x < sf.width; ++x) {
                const u32 my = sf.height - 1 - y;
                const u32 off = pica::pixel::morton_offset(x, my, bpp) + (my & ~7u) * sf.width * bpp;
                const auto c = pica::pixel::decode(sf.format, src + off);
                u8* d = out + (y * sf.width + x) * 4;
                d[0] = u8(c.r), d[1] = u8(c.g), d[2] = u8(c.b), d[3] = u8(c.a);
            }
    }

    void write_guest_color(Memory& mem, const Surface& sf, const u8* in) {
        const u32 bpp = pica::pixel::bytes_per_pixel(sf.format);
        u8* dst = mem.host(sf.va);
        for (u32 y = 0; y < sf.height; ++y)
            for (u32 x = 0; x < sf.width; ++x) {
                const u32 my = sf.height - 1 - y;
                const u32 off = pica::pixel::morton_offset(x, my, bpp) + (my & ~7u) * sf.width * bpp;
                const u8* p = in + (y * sf.width + x) * 4;
                pica::pixel::encode(sf.format, {p[0], p[1], p[2], p[3]}, dst + off);
            }
    }

    // Horizontal widening of the top screen relative to 5:3 (1 = off).
    float widescreen() const {
        float a = wide_aspect;
        if (a < 0) {
            const VkExtent2D e = ctx.extent();
            a = e.width && e.height ? float(e.width) / float(e.height) : 0;
        }
        return std::clamp(a * 3.0f / 5.0f, 1.0f, 4.0f);
    }

    u32 guest_bytes(const Surface& sf) const { return sf.width * sf.height * pica::pixel::bytes_per_pixel(sf.format); }
    u64 ram_hash(const Memory& mem, const Surface& sf) const { return hash_ram(mem.host(sf.va), guest_bytes(sf)); }

    void memory_barrier() {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }

    Image make_general(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        Image im = make_image(w, h, fmt, usage, aspect);
        begin();
        barrier(im.img, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        return im;
    }

    // Guest-size staging image between the transfer buffer and a scaled surface.
    Image& scratch_image(u32 w, u32 h) {
        Image& im = scratch[w << 16 | h];
        if (!im.img)
            im = make_general(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        return im;
    }

    void blit(VkImage src, u32 sw, u32 sh, VkImage dst, u32 dw, u32 dh, VkFilter filter) {
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {s32(sw), s32(sh), 1};
        b.dstOffsets[1] = {s32(dw), s32(dh), 1};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &b, filter);
    }

    // Guest memory -> surface (scaled up). Records into the open batch.
    void upload(const Memory& mem, Surface& sf) {
        const VkDeviceSize bytes = VkDeviceSize(sf.width) * sf.height * 4;
        if (x_used + align(bytes) > xfer.size) submit_and_wait();
        ensure(xfer, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Image& tmp = scratch_image(sf.width, sf.height);
        begin();
        const VkDeviceSize off = take(x_used, bytes);
        read_guest_color(mem, sf, xfer.ptr + off);
        memory_barrier();
        VkBufferImageCopy copy{};
        copy.bufferOffset = off;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {sf.width, sf.height, 1};
        vkCmdCopyBufferToImage(cmd, xfer.buf, tmp.img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        memory_barrier();
        blit(tmp.img, sf.width, sf.height, sf.color.img, sf.width * scale, sf.image_h, VK_FILTER_NEAREST);
        memory_barrier();
        sf.ram_hash = ram_hash(mem, sf);
        sf.gpu_newer = false;
    }

    // Surface -> guest memory (scaled down). Submits and waits.
    void readback(Memory& mem, Surface& sf) {
        if (!sf.gpu_newer) return;
        if (std::getenv("DOT_TRACE_FB"))
            std::fprintf(stderr, "renderer: readback %ux%u fmt %u at %08X\n", sf.width, sf.height, sf.format, sf.va);
        const VkDeviceSize bytes = VkDeviceSize(sf.width) * sf.height * 4;
        if (x_used + align(bytes) > xfer.size) submit_and_wait();
        ensure(xfer, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Image& tmp = scratch_image(sf.width, sf.height);
        begin();
        const VkDeviceSize off = take(x_used, bytes);
        memory_barrier();
        blit(sf.color.img, sf.width * scale, sf.image_h, tmp.img, sf.width, sf.height, VK_FILTER_LINEAR);
        memory_barrier();
        VkBufferImageCopy copy{};
        copy.bufferOffset = off;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {sf.width, sf.height, 1};
        vkCmdCopyImageToBuffer(cmd, tmp.img, VK_IMAGE_LAYOUT_GENERAL, xfer.buf, 1, &copy);
        submit_and_wait();
        const u8* const px = xfer.ptr + off;
        if (const char* dir = std::getenv("DOT_DUMP_TARGETS"); dir && sf.width >= 256) {
            static u32 n = 0;
            char name[512];
            std::snprintf(name, sizeof name, "%s/target_%04u_%08X.ppm", dir, n++, sf.va);
            if (std::FILE* f = std::fopen(name, "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", sf.width, sf.height);
                for (u32 y = sf.height; y-- > 0;)  // top row first in the image file
                    for (u32 x = 0; x < sf.width; ++x) std::fwrite(px + (y * sf.width + x) * 4, 1, 3, f);
                std::fclose(f);
            }
        }
        write_guest_color(mem, sf, px);
        sf.ram_hash = ram_hash(mem, sf);
        sf.gpu_newer = false;
        ++stats.readbacks;
    }

    void forget_framebuffers(u32 color_va, u32 depth_va) {
        for (auto it = framebuffers.begin(); it != framebuffers.end();)
            if (u32(it->first >> 32) == color_va || u32(it->first) == depth_va) {
                vkDestroyFramebuffer(dev, it->second, nullptr);
                it = framebuffers.erase(it);
            } else {
                ++it;
            }
    }

    // The cached surface for a colour buffer: created (from guest memory) on first use,
    // re-uploaded when guest memory changed behind the GPU's back.
    Surface& surface(Memory& mem, u32 va, u32 w, u32 h, u32 fmt) {
        const u32 image_h = wide_vas.count(va) ? u32(std::lround(h * scale * widescreen())) : h * scale;
        auto it = surfaces.find(va);
        if (it != surfaces.end() && (it->second.width != w || it->second.height != h || it->second.format != fmt ||
                                     it->second.image_h != image_h)) {
            if (std::getenv("DOT_TRACE_FB"))
                std::fprintf(stderr, "renderer: surface %08X %ux%u fmt %u reused as %ux%u fmt %u\n", va, it->second.width,
                             it->second.height, it->second.format, w, h, fmt);
            readback(mem, it->second);  // the memory is being reused with another layout
            submit_and_wait();
            forget_framebuffers(va, ~0u);
            destroy(it->second.color);
            if (cur == &it->second) cur = nullptr;
            surfaces.erase(it);
            it = surfaces.end();
        }
        Surface& sf = surfaces[va];
        if (!sf.color.img) {
            sf.va = va, sf.width = w, sf.height = h, sf.format = fmt, sf.image_h = image_h;
            if (std::getenv("DOT_TRACE_FB")) std::fprintf(stderr, "renderer: new surface %08X %ux%u fmt %u\n", va, w, h, fmt);
            sf.color = make_general(w * scale, image_h, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
            upload(mem, sf);
        } else if (ram_hash(mem, sf) != sf.ram_hash) {
            upload(mem, sf);  // the CPU wrote it: guest memory is the newest copy
        }
        return sf;
    }

    void clear_depth(Image& im, u32 va) {
        auto it = depth_clears.find(va);
        const VkClearDepthStencilValue v = it == depth_clears.end() ? VkClearDepthStencilValue{0.0f, 0}
                                                                    : VkClearDepthStencilValue{it->second.depth, it->second.stencil};
        const VkImageSubresourceRange range{DS_ASPECT, 0, 1, 0, 1};
        begin();
        memory_barrier();
        vkCmdClearDepthStencilImage(cmd, im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
        memory_barrier();
    }

    Depth& depth_buffer(u32 va, u32 w, u32 h, u32 image_h) {
        Depth& d = depths[va];
        if (d.image.img && (d.width != w || d.height != h || d.image_h != image_h)) {
            submit_and_wait();
            forget_framebuffers(~0u, va);
            destroy(d.image);
        }
        if (!d.image.img) {
            d.width = w, d.height = h, d.image_h = image_h;
            d.image = make_general(w * scale, image_h, depth_format,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, DS_ASPECT);
            clear_depth(d.image, va);
        }
        return d;
    }

    VkFramebuffer framebuffer(Surface& sf, u32 depth_va) {
        VkFramebuffer& fb = framebuffers[u64(sf.va) << 32 | depth_va];
        if (fb) return fb;
        Depth& d = depth_buffer(depth_va, sf.width, sf.height, sf.image_h);
        VkImageView views[2] = {sf.color.view, d.image.view};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = render_pass;
        fi.attachmentCount = 2;
        fi.pAttachments = views;
        fi.width = sf.width * scale, fi.height = sf.image_h, fi.layers = 1;
        vkCreateFramebuffer(dev, &fi, nullptr, &fb);
        return fb;
    }

    bool bind_target(Memory& mem, const std::array<u32, pica::reg::COUNT>& regs) {
        const u32 color_va = Memory::pa_to_va(regs[COLOR_ADDR] << 3);
        const u32 depth_va = Memory::pa_to_va(regs[DEPTH_ADDR] << 3);
        const u32 w = regs[FB_DIM] & 0x7FF, h = (regs[FB_DIM] >> 12 & 0x3FF) + 1;
        const u32 fmt = regs[COLOR_FORMAT] >> 16 & 7;
        if (!color_va || !w || fmt > 4) return false;
        if (cur && cur->va == color_va && cur_depth == depth_va && cur->width == w && cur->height == h &&
            cur->format == fmt && (!wide_vas.count(color_va) || cur->image_h == u32(std::lround(h * scale * widescreen()))))
            return true;
        cur = &surface(mem, color_va, w, h, fmt);
        cur_depth = depth_va;
        depth_buffer(depth_va, w, h, cur->image_h);  // may have been resized by another target
        return true;
    }

    void sync(Memory& mem, u32 start, u32 end, bool overwrite) {
        for (auto& [va, sf] : surfaces) {
            const u32 sf_end = va + guest_bytes(sf);
            if (!sf.gpu_newer || sf_end <= start || va >= end) continue;
            if (overwrite && va >= start && sf_end <= end) continue;
            readback(mem, sf);
        }
    }

    void fill(Memory& mem, u32 start, u32 end) {
        for (auto& [va, d] : depths)
            if (va == start) clear_depth(d.image, va);
        auto it = surfaces.find(start);
        if (it == surfaces.end() || start + guest_bytes(it->second) > end) return;
        Surface& sf = it->second;
        const auto c = pica::pixel::decode(sf.format, mem.host(start));  // the guest memory was just filled
        const VkClearColorValue v{{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        begin();
        memory_barrier();
        vkCmdClearColorImage(cmd, sf.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
        memory_barrier();
        sf.ram_hash = ram_hash(mem, sf);
        sf.gpu_newer = false;
    }

    bool display_transfer(Memory& mem, u32 in, u32 out, u32 in_dim, u32 out_dim, u32 flags) {
        const bool flip = flags & 1, input_linear = flags >> 1 & 1, dont_swizzle = flags >> 5 & 1;
        const u32 scaling = flags >> 24 & 3, out_fmt = flags >> 12 & 7;
        if (always_readback || input_linear || (flags >> 3 & 1) || scaling == 3) return false;
        // The source may start at a tile row inside a surface (the game renders both screens
        // into one buffer, one after the other, and transfers the bottom one from row 80).
        const u32 in_w = in_dim & 0xFFFF;
        Surface* src = nullptr;
        u32 row0 = 0;
        for (auto& [va, sf] : surfaces) {
            const u32 row_bytes = sf.width * pica::pixel::bytes_per_pixel(sf.format);
            if (in >= va && in < va + guest_bytes(sf) && sf.width == in_w && (in - va) % (row_bytes * 8) == 0) {
                src = &sf, row0 = (in - va) / row_bytes;
                break;
            }
        }
        if (!src) return false;
        Surface& sf = *src;
        if (ram_hash(mem, sf) != sf.ram_hash) upload(mem, sf);
        const u32 hs = scaling ? 1 : 0, vs = scaling == 2 ? 1 : 0;
        const u32 out_w = (out_dim & 0xFFFF) >> hs, out_h = (out_dim >> 16) >> vs;
        if (!out_w || !out_h || (out_w << hs) > sf.width || row0 + (out_h << vs) > sf.height) return false;
        const u32 rows_above = sf.height - row0;  // guest rows down to the first transferred memory row
        const float sy = sf.sy();
        // The top screen (400 framebuffer rows) is widened: its source surface gets more rows from the next frame.
        if (!dont_swizzle && out_h == 400 && widescreen() > 1.0f) wide_vas.insert(sf.va);
        if (dont_swizzle) {  // tiled -> tiled (downscaled copies used as textures): surface to surface
            if (out_fmt > 4 || out == in) return false;
            Surface& dst = surface(mem, out, out_w, out_h, out_fmt);
            // Memory row r is image row H-1-r on both sides: the copied rows are the source's last ones.
            const s32 S = s32(scale), sh = s32(std::lround(rows_above * sy)), rows = s32(std::lround((out_h << vs) * sy));
            VkImageBlit b{};
            b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.srcOffsets[0] = {0, sh - rows, 0};
            b.srcOffsets[1] = {s32(out_w << hs) * S, sh, 1};
            b.dstOffsets[0] = {0, flip ? s32(dst.image_h) : 0, 0};
            b.dstOffsets[1] = {s32(out_w) * S, flip ? 0 : s32(dst.image_h), 1};
            begin();
            memory_barrier();
            vkCmdBlitImage(cmd, sf.color.img, VK_IMAGE_LAYOUT_GENERAL, dst.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                           VK_FILTER_LINEAR);
            memory_barrier();
            dst.gpu_newer = true;
            return true;
        }
        // Screen orientation: one framebuffer row (out_w pixels) is one screen column.
        Display& d = displays[out];
        // Screen horizontal = surface rows: the top screen keeps the widened rows, others are resampled.
        const float src_cols = out_h * sy;
        const u32 screen_w = out_h == 400 ? u32(std::lround(src_cols)) : out_h * scale;
        if (replaying && out_h != 400) return true;  // only the top screen is interpolated
        if (d.image.img && (d.width != screen_w || d.height != out_w * scale)) {
            submit_and_wait();
            destroy(d.image), destroy(d.interp);
        }
        if (!d.image.img) {
            d.width = screen_w, d.height = out_w * scale;
            d.image = make_general(d.width, d.height, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        if (replaying && !d.interp.img)
            d.interp = make_general(d.width, d.height, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        const Image& dst_img = replaying ? d.interp : d.image;
        begin();
        VkDescriptorSet set;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = desc_pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &xfer_set_layout;
        if (vkAllocateDescriptorSets(dev, &dai, &set) != VK_SUCCESS) {
            submit_and_wait();
            begin();
            vkAllocateDescriptorSets(dev, &dai, &set);
        }
        const VkDescriptorImageInfo ii[2] = {{sampler(0), sf.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, dst_img.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &ii[0], nullptr, nullptr};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[1],
                nullptr, nullptr};
        vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
        memory_barrier();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, xfer_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, xfer_layout, 0, 1, &set, 0, nullptr);
        struct {
            s32 a[4];
            float b[4];
        } pc{{flip, s32(hs), s32(vs), s32(std::lround(rows_above * sy))}, {src_cols / float(screen_w), 0, 0, 0}};
        vkCmdPushConstants(cmd, xfer_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (d.width + 7) / 8, (d.height + 7) / 8, 1);
        memory_barrier();
        submit_and_wait();  // presentation (another thread) reads the image next
        if (replaying) {
            d.interp_fresh = true;
            return true;
        }
        d.interp_fresh = false;  // a new real frame: its in-between frame comes from the replay
        d.bytes = out_w * out_h * pica::pixel::bytes_per_pixel(out_fmt);
        d.ram_hash = hash_ram(mem.host(out), d.bytes);
        if (cache_dirty && std::chrono::steady_clock::now() - cache_saved > std::chrono::seconds(10)) save_caches();
        return true;
    }

    // Decodes and (re)uploads a texture unit if its contents changed. Records into cmd.
    struct TexSource {
        u32 va, w, h;
        pica::TexFormat fmt;
        size_t size;
    };
    static TexSource tex_source(const std::array<u32, pica::reg::COUNT>& regs, u32 unit) {
        const u32* r = TEX_REGS[unit];
        const u32 h = regs[r[0]] & 0x7FF, w = regs[r[0]] >> 16 & 0x7FF;
        const auto fmt = pica::TexFormat(regs[r[3]] & 0xF);
        return {Memory::pa_to_va(regs[r[2]] << 3), w, h, fmt, pica::texture_size(fmt, w, h)};
    }

    // Render-to-texture: a texture with a cached surface's exact address, size and (colour)
    // format samples the surface itself; any other overlap with a GPU-newer surface writes it
    // back to guest memory first. Must run before the draw takes batch memory (may submit).
    // A texture taller than the surface at its address (power-of-two padding) samples a
    // texture-sized copy with the surface in its first memory rows (the image's last rows).
    std::map<u32, Surface> padded;

    Surface* resolve_texture(Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, u32 unit) {
        const TexSource t = tex_source(regs, unit);
        if (!t.va || !t.size) return nullptr;
        auto it = surfaces.find(t.va);
        if (it != surfaces.end() && it->second.width == t.w && it->second.height <= t.h &&
            it->second.format == u32(t.fmt)) {
            Surface& sf = surface(mem, t.va, it->second.width, it->second.height, it->second.format);
            if (sf.height == t.h) return &sf;
            Surface& p = padded[t.va];
            if (p.color.img && (p.width != t.w || p.height != t.h)) submit_and_wait(), destroy(p.color);
            if (!p.color.img) {
                p.va = t.va, p.width = t.w, p.height = t.h, p.format = sf.format, p.image_h = t.h * scale;
                p.color = make_general(t.w * scale, p.image_h, VK_FORMAT_R8G8B8A8_UNORM,
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
            }
            const s32 S = s32(scale);
            VkImageBlit b{};
            b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.srcOffsets[1] = {s32(sf.width) * S, s32(sf.image_h), 1};
            b.dstOffsets[0] = {0, s32(t.h - sf.height) * S, 0};
            b.dstOffsets[1] = {s32(t.w) * S, s32(t.h) * S, 1};
            begin();
            memory_barrier();
            vkCmdBlitImage(cmd, sf.color.img, VK_IMAGE_LAYOUT_GENERAL, p.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                           VK_FILTER_NEAREST);
            memory_barrier();
            return &p;
        }
        if (std::getenv("DOT_TRACE_FB"))
            for (auto& [va, sf] : surfaces)
                if (sf.gpu_newer && va < t.va + t.size && t.va < va + guest_bytes(sf))
                    std::fprintf(stderr, "renderer: texture %ux%u fmt %u at %08X overlaps surface %ux%u fmt %u at %08X\n",
                                 t.w, t.h, u32(t.fmt), t.va, sf.width, sf.height, sf.format, va);
        sync(mem, t.va, t.va + u32(t.size), false);
        return nullptr;
    }

    Texture surface_tex[3];

    Texture& texture(const Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, u32 unit, Surface* from) {
        if (from) {
            Texture& t = surface_tex[unit];
            t.image.view = from->color.view, t.layout = VK_IMAGE_LAYOUT_GENERAL;
            return t;
        }
        const auto [va, w, h, fmt, size] = tex_source(regs, unit);
        if (!va || !w || !h || !size || w % 8 || h % 8) return dummy;
        const u64 key = fnv(&va, 4, fnv(&w, 4, fnv(&h, 4, u64(fmt))));
        Texture& t = textures[key];
        const u64 hash = hash_words(mem.host(va), size);
        if (t.ready && t.hash == hash) return t;
        const auto rgba = pica::decode_texture({mem.host(va), size}, w, h, fmt);
        if (std::getenv("DOT_TRACE_DRAWS")) {
            size_t nonzero = 0;
            for (u32 px : rgba) nonzero += (px & 0xFFFFFF) != 0;
            std::fprintf(stderr, "renderer: texture unit %u %ux%u format %u at %08X: %zu non-black texels\n", unit, w,
                         h, u32(fmt), va, nonzero);
        }
        if (rgba.empty()) return dummy;
        if (!t.image.img) {
            t.image = make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            t.width = w, t.height = h;
        }
        // Texture memory is bottom-up too: memory row r is t = (h - 1 - r) / h, so flip rows
        // to make Vulkan's v coordinate equal PICA's t.
        const VkDeviceSize off = take(x_used, rgba.size() * 4);
        for (u32 row = 0; row < h; ++row)
            std::memcpy(xfer.ptr + off + size_t(row) * w * 4, rgba.data() + size_t(h - 1 - row) * w, w * 4);
        upload_image(t.image.img, w, h, off);
        t.hash = hash, t.ready = true;
        return t;
    }

    VkPipeline pipeline(VkShaderModule vs, VkShaderModule fs, const std::array<u32, pica::reg::COUNT>& regs) {
        const u32 topo = regs[PRIMITIVE_CONFIG] >> 8 & 3;
        const bool stencil = has_stencil(regs) && (regs[STENCIL_TEST] & 1);
        const u32 state[] = {topo, regs[CULL] & 3, regs[COLOR_OP], regs[BLEND_FUNC], regs[DEPTH_COLOR_MASK],
                             regs[COLOR_WRITE_ENABLE] != 0, regs[DEPTH_WRITE_ENABLE] != 0,
                             stencil ? (regs[STENCIL_TEST] & 0x70) | (regs[STENCIL_OP] & 0x777) << 8 | 1 : 0};
        const u64 key = fnv(state, sizeof state, fnv(&vs, sizeof vs, fnv(&fs, sizeof fs)));
        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs,
                     "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs,
                     "main", nullptr};
        // GPU vertex shaders read the raw inputs v0..v15; the pass-through one a ShadedVertex.
        const bool raw = vs != pass_vs;
        const u32 nattr = raw ? 16 : 6;
        VkVertexInputBindingDescription binding{
            0, raw ? u32(sizeof(pica::VertexInput)) : u32(sizeof(ShadedVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[16];
        for (u32 i = 0; i < nattr; ++i) attrs[i] = {i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = nattr, vi.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = topo == 1   ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                      : topo == 2 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // PICA culls in GL terms (1: counter-clockwise, 2: clockwise); with the same window
        // coordinates Vulkan's winding is mirrored, so GL-CCW is Vulkan-CW.
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        const u32 cull = regs[CULL] & 3;
        rs.cullMode = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        const u32 dcm = regs[DEPTH_COLOR_MASK];
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = dcm & 1;
        ds.depthCompareOp = compare_op(dcm >> 4);
        ds.depthWriteEnable = (dcm >> 12 & 1) && regs[DEPTH_WRITE_ENABLE] != 0;
        // Reference and masks are dynamic state (set per draw).
        ds.stencilTestEnable = stencil;
        ds.front.failOp = stencil_op(regs[STENCIL_OP]);
        ds.front.depthFailOp = stencil_op(regs[STENCIL_OP] >> 4);
        ds.front.passOp = stencil_op(regs[STENCIL_OP] >> 8);
        ds.front.compareOp = compare_op(regs[STENCIL_TEST] >> 4);
        ds.back = ds.front;
        VkPipelineColorBlendAttachmentState cb{};
        const u32 bf = regs[BLEND_FUNC];
        cb.blendEnable = regs[COLOR_OP] >> 8 & 1;  // otherwise logic op (shortcut: treated as COPY)
        cb.colorBlendOp = blend_op(bf & 7), cb.alphaBlendOp = blend_op(bf >> 8 & 7);
        cb.srcColorBlendFactor = blend_factor(bf >> 16 & 0xF), cb.dstColorBlendFactor = blend_factor(bf >> 20 & 0xF);
        cb.srcAlphaBlendFactor = blend_factor(bf >> 24 & 0xF), cb.dstAlphaBlendFactor = blend_factor(bf >> 28 & 0xF);
        cb.colorWriteMask = regs[COLOR_WRITE_ENABLE] ? (dcm >> 8 & 0xF) : 0;
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1, cbs.pAttachments = &cb;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 6, dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.stageCount = 2, pi.pStages = stages;
        pi.pVertexInputState = &vi, pi.pInputAssemblyState = &ia, pi.pViewportState = &vp;
        pi.pRasterizationState = &rs, pi.pMultisampleState = &ms, pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cbs, pi.pDynamicState = &dy;
        pi.layout = pipeline_layout, pi.renderPass = render_pass;
        VkPipeline p = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev, pipeline_cache, 1, &pi, nullptr, &p), "vkCreateGraphicsPipelines");
        pipelines[key] = p;
        ++stats.pipelines;
        cache_dirty = true;
        return p;
    }

    // The draw's vertex program as a GPU shader specialised on the bool/int uniforms, or null
    // when it cannot be structured (then it runs on the CPU).
    VkShaderModule vertex_shader(const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs, u32 entry) {
        u64 key = fnv(vs.code.data(), sizeof vs.code, fnv(vs.swizzle.data(), sizeof vs.swizzle));
        key = fnv(&regs[OUTMAP], 7 * 4, fnv(vs.int_uniforms.data(), 16, fnv(&vs.bool_uniforms, 2, fnv(&entry, 4, key))));
        auto it = vertex_shaders.find(key);
        if (it != vertex_shaders.end()) return it->second;
        std::string err;
        const std::string glsl = pica::generate_vertex_shader(
            {vs.code, vs.swizzle, entry, {&regs[OUTMAP], 7}, vs.bool_uniforms, vs.int_uniforms}, &err);
        if (glsl.empty()) std::fprintf(stderr, "renderer: vertex program %u on the CPU: %s\n", entry, err.c_str());
        return vertex_shaders[key] = glsl.empty() ? VK_NULL_HANDLE : module(glsl, ShaderStage::Vertex);
    }

    void record_draw(const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs, bool indexed) {
        if (!interpolate || replaying) return;
        if (!last_vs || std::memcmp(last_vs.get(), &vs, sizeof vs)) last_vs = std::make_shared<pica::ShaderState>(vs);
        Op op{Op::Draw};
        op.indexed = indexed, op.regs = regs, op.vs = last_vs;
        // What is drawn: program entry, attribute buffers, index buffer and count, texture 0.
        const u32 what[] = {regs[VS_ENTRY] & 0xFFFF, regs[0x200], regs[0x203], regs[0x227], regs[0x228], regs[0x085],
                            regs[COLOR_ADDR]};
        const u64 k = fnv(what, sizeof what);
        op.key = k ^ u64(key_counts[k]++) * 0x9E3779B97F4A7C15ull;
        frame_ops.push_back(std::move(op));
    }

    void record_op(Op::Kind kind, std::initializer_list<u32> args, bool gpu) {
        if (!interpolate || replaying) return;
        Op op{kind};
        op.gpu = gpu;
        u32 i = 0;
        for (u32 v : args) op.a[i++] = v;
        frame_ops.push_back(std::move(op));
    }

    // End of a top-screen frame (its transfer was just recorded and run): replay it halfway
    // between the previous frame and this one.
    void end_frame(Memory& mem) {
        if (!interpolate || replaying) return;
        std::map<u64, const Op*> prev;
        for (const Op& op : prev_ops)
            if (op.kind == Op::Draw) prev[op.key] = &op;
        u32 draws = 0, matched = 0;
        for (const Op& op : frame_ops)
            if (op.kind == Op::Draw) ++draws, matched += prev.count(op.key);
        // A camera cut or scene change matches few draws: show the real frame twice instead.
        if (draws && matched * 2 >= draws) {
            replaying = true;
            pica::ShaderState vs;
            for (const Op& op : frame_ops) {
                switch (op.kind) {
                case Op::Draw: {
                    vs = *op.vs;
                    if (auto it = prev.find(op.key); it != prev.end())
                        for (u32 i = 0; i < 96; ++i) {
                            const pica::Vec4& a = it->second->vs->float_uniforms[i];
                            pica::Vec4& b = vs.float_uniforms[i];
                            b = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f, (a.w + b.w) * 0.5f};
                        }
                    draw(mem, op.regs, vs, op.indexed);
                    break;
                }
                case Op::Fill:
                    if (op.gpu) fill(mem, op.a[0], op.a[1]);
                    break;
                case Op::Transfer:
                    if (op.gpu) display_transfer(mem, op.a[0], op.a[1], op.a[2], op.a[3], op.a[4]);
                    break;
                }
            }
            replaying = false;
            ++interp_frames;
            const auto now = std::chrono::steady_clock::now();
            for (auto& [va, d] : displays)
                if (d.interp_fresh) {
                    d.period = std::clamp<std::chrono::steady_clock::duration>(now - d.ready, std::chrono::microseconds(16'715),
                                                                                std::chrono::milliseconds(100));
                    d.ready = now;
                }
        } else {
            ++interp_skipped;
        }
        prev_ops.swap(frame_ops);
        frame_ops.clear();
        key_counts.clear();
    }

    // Fragment shader text depends only on these registers (shader_gen.cpp: texture unit
    // config, TEV stages without their constant colours, fog mode, alpha test, lighting
    // structure); per-draw values travel as push constants or in the lighting buffer.
    // DOT_VERIFY_FS=1 regenerates every time and reports a mismatch with the memoised one.
    std::map<u64, VkShaderModule> fs_memo;
    VkShaderModule fragment_shader(const std::array<u32, pica::reg::COUNT>& regs) {
        u32 key_regs[64];
        u32 n = 0;
        for (u32 r : {0x080u, 0x08Fu, 0x090u, 0x0E0u, 0x104u, 0x1C2u, 0x1C3u, 0x1C4u, 0x1C6u, 0x1D0u, 0x1D1u, 0x1D2u, 0x1D9u})
            key_regs[n++] = regs[r];
        for (u32 base : {0xC0u, 0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u})
            for (u32 k = 0; k < 5; ++k)
                if (k != 3) key_regs[n++] = regs[base + k];
        for (u32 l = 0; l < 8; ++l) key_regs[n++] = regs[0x149 + 16 * l];
        const u64 key = fnv(key_regs, n * 4);
        static const bool verify = std::getenv("DOT_VERIFY_FS") != nullptr;
        auto it = fs_memo.find(key);
        if (it != fs_memo.end() && !verify) return it->second;
        const VkShaderModule m = module(pica::generate_fragment_shader(regs), ShaderStage::Fragment);
        if (verify && it != fs_memo.end() && it->second != m) std::fprintf(stderr, "renderer: fragment shader memo MISMATCH\n");
        return fs_memo[key] = m;
    }

    // Clip-space y factor for a draw (u.depth.z). On a widened top-screen surface every draw
    // covering the whole screen (viewport rows 0..height) is squeezed back by 1/W, so 3D
    // perspective gains field of view and 2D elements keep their shape, centred; orthographic
    // draws spanning the full width (fades, full-screen composites) stay stretched to the edges.
    // Projection is told apart by running the vertex program on the CPU for a few vertices.
    float widescreen_clip(const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs,
                          const std::vector<pica::VertexInput>& verts) {
        if (cur->image_h == cur->height * scale) return 1.0f;
        const float vh = f24_to_f32(regs[VIEWPORT_H] & 0xFFFFFF) * 2;
        if ((regs[VIEWPORT_XY] >> 16 & 0x3FF) != 0 || std::lround(vh) != long(cur->height)) return 1.0f;
        const float squeeze = float(cur->height * scale) / float(cur->image_h);
        const u32 entry = regs[VS_ENTRY] & 0xFFFF;
        auto clip_yw = [&](size_t n, float& y, float& w) {
            pica::Vec4 out[16];
            pica::run_vertex_shader(vs, entry, verts[n].v, out);
            y = 0, w = 1;
            for (u32 reg = 0; reg < 7; ++reg)
                for (u32 c = 0; c < 4; ++c) {
                    const u32 id = regs[OUTMAP + reg] >> (c * 8) & 0x1F;
                    if (id == 1) y = (&out[reg].x)[c];
                    if (id == 3) w = (&out[reg].x)[c];
                }
        };
        float y, w;
        for (size_t n = 0; n < std::min<size_t>(3, verts.size()); ++n) {
            clip_yw(n, y, w);
            if (std::fabs(w - 1.0f) > 1e-3f) return squeeze;  // perspective
        }
        if (verts.size() > 64) return squeeze;  // orthographic geometry, not a screen quad
        float lo = 1e9f, hi = -1e9f;
        for (size_t n = 0; n < verts.size(); ++n) {
            clip_yw(n, y, w);
            lo = std::min(lo, y / w), hi = std::max(hi, y / w);
        }
        return lo <= -0.98f && hi >= 0.98f ? 1.0f : squeeze;
    }

    void draw(Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs, bool indexed) {
        auto skip = [&](const char* why) {
            static std::map<std::string, bool> seen;
            if (!seen[why]) std::fprintf(stderr, "renderer: skipping draws: %s\n", why), seen[why] = true;
            ++stats.skipped;
        };
        if (!bind_target(mem, regs)) return skip("unsupported or missing colour buffer");
        Surface* tex_surface[3] = {};
        for (u32 i = 0; i < 3; ++i)
            if (regs[TEX_CONFIG] >> i & 1) tex_surface[i] = resolve_texture(mem, regs, i);
        if (!cur) return skip("render target lost");  // a resolved texture replaced it

        std::vector<pica::VertexInput> verts;
        if (!pica::load_vertices(mem, regs, vs, indexed, verts) || verts.empty()) return skip("vertex fetch");

        // Vertex shader on the GPU when possible, else run on the CPU with the outputs packed
        // through VS_OUTMAP for the pass-through shader.
        const u32 entry = regs[VS_ENTRY] & 0xFFFF;
        const VkShaderModule gpu_vs = vertex_shader(regs, vs, entry);
        std::vector<ShadedVertex> shaded(gpu_vs ? 0 : verts.size());
        if (!gpu_vs) {
            ShadedVertex sv{};
            float* const p = sv.pos;  // indices into the packed vertex, by semantic id
            const int offs[0x18] = {0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 8, 9, 10, 11, 14, -1, 20, 21, 22, -1, 12, 13};
            for (size_t n = 0; n < verts.size(); ++n) {
                pica::Vec4 out[16];
                pica::run_vertex_shader(vs, entry, verts[n].v, out);
                sv = {};
                sv.pos[3] = 1.0f;
                for (u32 reg = 0; reg < 7; ++reg)
                    for (u32 c = 0; c < 4; ++c) {
                        const u32 id = regs[OUTMAP + reg] >> (c * 8) & 0x1F;
                        if (id < 0x18 && offs[id] >= 0) p[offs[id]] = (&out[reg].x)[c];
                    }
                shaded[n] = sv;
            }
        }
        if (!pass_vs) pass_vs = module(PASS_VS, ShaderStage::Vertex);
        const VkShaderModule vmod = gpu_vs ? gpu_vs : pass_vs;
        const VkShaderModule fmod = fragment_shader(regs);
        if (!vmod || !fmod) return skip("shader compile");
        const VkPipeline pipe = pipeline(vmod, fmod, regs);
        if (!pipe) return skip("pipeline creation");

        // Room in this batch: vertices, uniforms, lighting, staging (texture uploads are
        // bounded by the units' full sizes, the target upload by its size).
        const void* const vdata = gpu_vs ? static_cast<const void*>(verts.data()) : shaded.data();
        const VkDeviceSize vbytes = gpu_vs ? verts.size() * sizeof(pica::VertexInput) : shaded.size() * sizeof(ShadedVertex);
        VkDeviceSize xbytes = 0;
        for (u32 i = 0; i < 3; ++i)
            if (regs[TEX_CONFIG] >> i & 1) {
                const u32 dim = regs[TEX_REGS[i][0]];
                xbytes += align(VkDeviceSize(dim & 0x7FF) * (dim >> 16 & 0x7FF) * 4);
            }
        if (batch_draws == MAX_BATCH_DRAWS || vb_used + align(vbytes) > vbuf.size ||
            ub_used + align(UBO_SIZE) > ubuf.size || lb_used + align(LIGHT_BUF_SIZE) > lbuf.size ||
            x_used + xbytes > xfer.size)
            submit_and_wait();
        ensure(vbuf, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);  // grows only right after a submit
        ensure(xfer, xbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        ++batch_draws;

        // Vertex data and uniforms.
        const VkDeviceSize voff = take(vb_used, vbytes), uoff = take(ub_used, UBO_SIZE);
        std::memcpy(vbuf.ptr + voff, vdata, vbytes);
        u8* u = ubuf.ptr + uoff;
        std::memcpy(u, vs.float_uniforms.data(), 96 * 16);
        for (u32 i = 0; i < 4; ++i) {
            const u32 iv = vs.int_uniforms[i];
            const s32 v[4] = {s32(iv & 0xFF), s32(iv >> 8 & 0xFF), s32(s8(iv >> 16)), 0};
            std::memcpy(u + 1536 + i * 16, v, 16);
        }
        const float depth[4] = {f24_to_f32(regs[DEPTH_SCALE] & 0xFFFFFF), f24_to_f32(regs[DEPTH_OFFSET] & 0xFFFFFF),
                                widescreen_clip(regs, vs, verts), 0};
        std::memcpy(u + 1600, depth, 16);
        const u32 bools = vs.bool_uniforms;
        std::memcpy(u + 1616, &bools, 4);
        // Lighting registers + LUTs rarely change between draws: reuse the last copy if equal.
        if (last_light.empty() || std::memcmp(last_light.data(), &regs[LIGHT_REGS], LIGHT_REG_WORDS * 4) ||
            std::memcmp(last_light.data() + LIGHT_REG_WORDS, vs.light_lut.data(), sizeof(vs.light_lut)) ||
            std::memcmp(last_light.data() + LIGHT_REG_WORDS + vs.light_lut.size(), vs.fog_lut.data(),
                        sizeof(vs.fog_lut))) {
            last_light.assign(&regs[LIGHT_REGS], &regs[LIGHT_REGS] + LIGHT_REG_WORDS);
            last_light.insert(last_light.end(), vs.light_lut.begin(), vs.light_lut.end());
            last_light.insert(last_light.end(), vs.fog_lut.begin(), vs.fog_lut.end());
            lb_last = take(lb_used, LIGHT_BUF_SIZE);
            std::memcpy(lbuf.ptr + lb_last, last_light.data(), LIGHT_BUF_SIZE);
        }

        begin();
        // Texture uploads (before the render pass).
        Texture* tex[3];
        for (u32 i = 0; i < 3; ++i)
            tex[i] = regs[TEX_CONFIG] >> i & 1 ? &texture(mem, regs, i, tex_surface[i]) : &dummy;
        if (tex_surface[0] || tex_surface[1] || tex_surface[2]) memory_barrier();  // earlier passes wrote them
        const VkFramebuffer fb = framebuffer(*cur, cur_depth);

        // Descriptors (the pool is recycled per batch).
        VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = desc_pool;
        dai.descriptorSetCount = 2;
        dai.pSetLayouts = set_layouts;
        vkAllocateDescriptorSets(dev, &dai, sets);
        const VkDescriptorBufferInfo bi[2] = {{ubuf.buf, uoff, UBO_SIZE}, {lbuf.buf, lb_last, LIGHT_BUF_SIZE}};
        VkDescriptorImageInfo ii[3];
        for (u32 i = 0; i < 3; ++i)
            ii[i] = {sampler(regs[TEX_REGS[i][1]]), tex[i]->image.view, tex[i]->layout};
        VkWriteDescriptorSet w[3]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[0], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                nullptr, &bi[0], nullptr};
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[0], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr, &bi[1], nullptr};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[1], 0, 0, 3,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii, nullptr, nullptr};
        vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);

        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = render_pass;
        rbi.framebuffer = fb;
        rbi.renderArea = {{0, 0}, {cur->width * scale, cur->image_h}};
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        cur->gpu_newer = true;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        const float S = float(scale);
        const float vw = f24_to_f32(regs[VIEWPORT_W] & 0xFFFFFF) * 2, vh = f24_to_f32(regs[VIEWPORT_H] & 0xFFFFFF) * 2;
        const float SY = cur->sy();
        VkViewport viewport{float(regs[VIEWPORT_XY] & 0x3FF) * S, float(regs[VIEWPORT_XY] >> 16 & 0x3FF) * SY, vw * S,
                            vh * SY, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {cur->width * scale, cur->image_h}};
        if ((regs[SCISSOR_MODE] & 3) == 3) {
            const s32 x1 = regs[SCISSOR_MIN] & 0x3FF, y1 = regs[SCISSOR_MIN] >> 16 & 0x3FF;
            const s32 x2 = regs[SCISSOR_MAX] & 0x3FF, y2 = regs[SCISSOR_MAX] >> 16 & 0x3FF;
            const s32 k = s32(scale), ya = s32(std::floor(y1 * SY)), yb = s32(std::ceil((y2 + 1) * SY));
            scissor = {{x1 * k, ya}, {u32(std::max(0, x2 - x1 + 1) * k), u32(std::max(0, yb - ya))}};
        }
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        const u32 bc = regs[BLEND_COLOR];
        const float consts[4] = {(bc & 0xFF) / 255.f, (bc >> 8 & 0xFF) / 255.f, (bc >> 16 & 0xFF) / 255.f, (bc >> 24) / 255.f};
        vkCmdSetBlendConstants(cmd, consts);
        const u32 st = regs[STENCIL_TEST];
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, st >> 24 & 0xFF);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                 regs[DEPTH_WRITE_ENABLE] & 1 ? st >> 8 & 0xFF : 0);  // bit 0 stencil, bit 1 depth
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, st >> 16 & 0xFF);
        const auto push = pica::fragment_push_constants(regs);
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push.data());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, sets, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf.buf, &voff);
        vkCmdDraw(cmd, u32(verts.size()), 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        ++stats.draws;
        if (std::getenv("DOT_TRACE_FB"))
            std::fprintf(stderr, "renderer:   draw %zu vertices into %08X\n", verts.size(), cur->va);
    }
};

Renderer::Renderer(Context& ctx) : impl_(std::make_unique<Impl>(ctx)) {}
Renderer::~Renderer() {
    std::lock_guard lk(impl_->lock);
    impl_->save_caches();
}
bool Renderer::init(const std::filesystem::path& cache_dir) { return impl_->init(cache_dir); }

template <class F> static void timed(const char* what, F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 30 && std::getenv("DOT_TRACE_DRAWS")) std::fprintf(stderr, "renderer: slow %s %.0f ms\n", what, ms);
}

void Renderer::draw(core::Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs,
                    bool indexed) {
    std::lock_guard lk(impl_->lock);
    impl_->record_draw(regs, vs, indexed);
    timed("draw (total)", [&] { impl_->draw(mem, regs, vs, indexed); });
}

void Renderer::sync(core::Memory& mem, u32 start, u32 end, bool overwrite) {
    std::lock_guard lk(impl_->lock);
    timed("sync", [&] { impl_->sync(mem, start, end, overwrite); });
}

void Renderer::memory_fill(core::Memory& mem, u32 start, u32 end, u32 value, u32 width_mode) {
    std::lock_guard lk(impl_->lock);
    // 16-bit fills clear D16; 24/32-bit fills D24, the 32-bit ones with the stencil in the top byte.
    impl_->depth_clears[start] = width_mode == 0 ? Impl::DepthClear{(value & 0xFFFF) / 65535.0f, 0}
                                                 : Impl::DepthClear{(value & 0xFFFFFF) / 16777215.0f, width_mode == 2 ? value >> 24 : 0};
    impl_->record_op(Impl::Op::Fill, {start, end}, true);
    impl_->fill(mem, start, end);
}

bool Renderer::display_transfer(core::Memory& mem, u32 in, u32 out, u32 in_dim, u32 out_dim, u32 flags) {
    std::lock_guard lk(impl_->lock);
    const bool gpu = impl_->display_transfer(mem, in, out, in_dim, out_dim, flags);
    impl_->record_op(Impl::Op::Transfer, {in, out, in_dim, out_dim, flags}, gpu);
    // A transfer to the top screen's framebuffer (400 rows, linear) ends a game frame.
    const u32 scaling = flags >> 24 & 3, rows = (out_dim >> 16) >> (scaling == 2 ? 1 : 0);
    if (gpu && !(flags >> 5 & 1) && rows == 400) impl_->end_frame(mem);
    return gpu;
}

bool Renderer::display(core::Memory& mem, u32 va, GpuScreen& out, bool consume) {
    std::lock_guard lk(impl_->lock);
    auto it = impl_->displays.find(va);
    if (it == impl_->displays.end() || !it->second.image.img) return false;
    Impl::Display& d = it->second;
    if (hash_ram(mem.host(va), d.bytes) != d.ram_hash) return false;  // the CPU wrote the framebuffer since
    // The in-between frame stays up for half the time between real frames, then the real one:
    // even pacing at any monitor refresh, also when the game drops to 20 or 15 frames/s.
    if (consume && d.interp_fresh) {
        if (std::chrono::steady_clock::now() - d.ready < d.period / 2) {
            out = {d.interp.img, d.width, d.height};
            return true;
        }
        d.interp_fresh = false;
    }
    out = {d.image.img, d.width, d.height};
    return true;
}

bool Renderer::read_display(u32 va, std::vector<u32>& rgba, u32& width, u32& height, bool interp) {
    std::lock_guard lk(impl_->lock);
    Impl& m = *impl_;
    auto it = m.displays.find(va);
    if (it == m.displays.end() || !(interp ? it->second.interp : it->second.image).img) return false;
    const Impl::Display& d = it->second;
    const VkDeviceSize bytes = VkDeviceSize(d.width) * d.height * 4;
    m.submit_and_wait();
    m.ensure(m.xfer, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    m.begin();
    m.memory_barrier();
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {d.width, d.height, 1};
    vkCmdCopyImageToBuffer(m.cmd, (interp ? d.interp : d.image).img, VK_IMAGE_LAYOUT_GENERAL, m.xfer.buf, 1, &copy);
    m.submit_and_wait();
    width = d.width, height = d.height;
    rgba.resize(size_t(width) * height);
    std::memcpy(rgba.data(), m.xfer.ptr, bytes);
    return true;
}

u32 Renderer::scale() const { return impl_->scale; }

void Renderer::set_widescreen(float aspect) {
    std::lock_guard lk(impl_->lock);
    impl_->wide_aspect = aspect;
}

void Renderer::set_interpolate(bool on) {
    std::lock_guard lk(impl_->lock);
    impl_->interpolate = on;
    impl_->frame_ops.clear(), impl_->prev_ops.clear(), impl_->key_counts.clear();
    for (auto& [va, d] : impl_->displays) d.interp_fresh = false;
}

Renderer::Stats Renderer::stats() const {
    Stats s = impl_->stats;
    s.interpolated = impl_->interp_frames, s.not_interpolated = impl_->interp_skipped;
    return s;
}

}  // namespace gfx::vulkan
