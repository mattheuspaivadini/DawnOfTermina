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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
constexpr u32 TEX_CONFIG = 0x080, COLOR_OP = 0x100, BLEND_FUNC = 0x101, BLEND_COLOR = 0x103;
constexpr u32 DEPTH_COLOR_MASK = 0x107, COLOR_WRITE_ENABLE = 0x113, DEPTH_WRITE_ENABLE = 0x115;
constexpr u32 COLOR_FORMAT = 0x117, DEPTH_ADDR = 0x11C, COLOR_ADDR = 0x11D, FB_DIM = 0x11E;
constexpr u32 PRIMITIVE_CONFIG = 0x25E, VS_ENTRY = 0x2BA;
// Texture units: dims, param, address, type.
constexpr u32 TEX_REGS[3][4] = {{0x082, 0x083, 0x085, 0x08E}, {0x092, 0x093, 0x095, 0x096}, {0x09A, 0x09B, 0x09D, 0x09E}};

constexpr u32 UBO_SIZE = 96 * 16 + 4 * 16 + 16 + 16;
constexpr u32 LIGHT_REGS = 0x140, LIGHT_REG_WORDS = 0x90;  // fragment lighting buffer, see shader_gen.h
constexpr u32 LIGHT_BUF_SIZE = (LIGHT_REG_WORDS + 24 * 256) * 4;

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
    // PICA window depth = z/w * scale + offset; pre-multiplied by w for Vulkan.
    gl_Position = vec4(in_pos.x, in_pos.y, in_pos.z * u.depth.x + in_pos.w * u.depth.y, in_pos.w);
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
    struct Target {
        u32 color_va = 0, depth_va = 0, width = 0, height = 0, format = 0;
        Image color, depth;
        VkFramebuffer fb = VK_NULL_HANDLE;
        bool fresh = true;  // needs the guest pixels uploaded + depth initialised
    };
    struct Texture {
        Image image;
        u32 width = 0, height = 0;
        u64 hash = 0;
        bool ready = false;
    };

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layouts[2]{};
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    Buffer vbuf, ubuf, lbuf, xfer;
    Target target;
    Texture dummy;
    std::map<u64, Texture> textures;  // by (va, size, format)
    std::map<u32, VkSampler> samplers;
    std::map<u64, VkShaderModule> modules;
    VkShaderModule pass_vs = VK_NULL_HANDLE;
    std::map<u64, VkPipeline> pipelines;
    std::map<u32, float> depth_clears;  // last MemoryFill value per address, normalised
    std::mutex lock;  // draw/flush may come from different guest threads
    Stats stats{};

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

    void begin() {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
    }

    void submit_and_wait() {
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
    }

    bool init() {
        dev = ctx.device(), gpu = ctx.gpu();
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

        // Render pass: the colour/depth targets keep their contents between draws.
        VkAttachmentDescription att[2]{};
        att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        att[1].format = VK_FORMAT_D32_SFLOAT;
        for (int i = 0; i < 2; ++i) {
            att[i].samples = VK_SAMPLE_COUNT_1_BIT;
            att[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[i].initialLayout = att[i].finalLayout =
                i ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
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
        vkCreatePipelineLayout(dev, &pli, nullptr, &pipeline_layout);
        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 4;
        dpi.poolSizeCount = 3;
        dpi.pPoolSizes = sizes;
        vkCreateDescriptorPool(dev, &dpi, nullptr, &desc_pool);

        ensure(vbuf, 1 << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        ensure(ubuf, UBO_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        ensure(lbuf, LIGHT_BUF_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        ensure(xfer, 1 << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

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
        const auto spv = compile_glsl(glsl, stage, &log);
        VkShaderModule m = VK_NULL_HANDLE;
        if (std::getenv("DOT_TRACE_SHADERS") && stage == ShaderStage::Fragment)
            std::fprintf(stderr, "---- new fragment shader ----\n%s\n", glsl.c_str());
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
        VkSampler s;
        vkCreateSampler(dev, &si, nullptr, &s);
        samplers[key] = s;
        return s;
    }

    // Guest colour buffer (tiled, guest format) <-> Vulkan image rows (window y). PICA lays
    // colour buffers out bottom-up: memory row r holds window row H - 1 - r.
    void read_guest_color(const Memory& mem, u8* out) {
        const u32 bpp = pica::pixel::bytes_per_pixel(target.format);
        const u8* src = mem.host(target.color_va);
        for (u32 y = 0; y < target.height; ++y)
            for (u32 x = 0; x < target.width; ++x) {
                const u32 my = target.height - 1 - y;
                const u32 off = pica::pixel::morton_offset(x, my, bpp) + (my & ~7u) * target.width * bpp;
                const auto c = pica::pixel::decode(target.format, src + off);
                u8* d = out + (y * target.width + x) * 4;
                d[0] = u8(c.r), d[1] = u8(c.g), d[2] = u8(c.b), d[3] = u8(c.a);
            }
    }

    void write_guest_color(Memory& mem, const u8* in) {
        const u32 bpp = pica::pixel::bytes_per_pixel(target.format);
        u8* dst = mem.host(target.color_va);
        for (u32 y = 0; y < target.height; ++y)
            for (u32 x = 0; x < target.width; ++x) {
                const u32 my = target.height - 1 - y;
                const u32 off = pica::pixel::morton_offset(x, my, bpp) + (my & ~7u) * target.width * bpp;
                const u8* s = in + (y * target.width + x) * 4;
                pica::pixel::encode(target.format, {s[0], s[1], s[2], s[3]}, dst + off);
            }
    }

    void drop_target() {
        if (!target.color.img) return;
        vkDestroyFramebuffer(dev, target.fb, nullptr);
        destroy(target.color);
        destroy(target.depth);
        target = {};
    }

    void flush(Memory& mem) {
        if (!target.color.img) return;
        if (!target.fresh) {  // only write back if something was drawn
            const VkDeviceSize bytes = VkDeviceSize(target.width) * target.height * 4;
            ensure(xfer, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            begin();
            barrier(target.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {target.width, target.height, 1};
            vkCmdCopyImageToBuffer(cmd, target.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, xfer.buf, 1, &copy);
            submit_and_wait();
            if (std::getenv("DOT_TRACE_DRAWS") || std::getenv("DOT_TRACE_FB")) {
                size_t nonzero = 0;
                for (VkDeviceSize i = 0; i < bytes; i += 4) nonzero += (xfer.ptr[i] | xfer.ptr[i + 1] | xfer.ptr[i + 2]) != 0;
                std::fprintf(stderr, "renderer: flush %ux%u fmt %u at %08X: %zu non-black pixels\n", target.width,
                             target.height, target.format, target.color_va, nonzero);
            }
            if (const char* dir = std::getenv("DOT_DUMP_TARGETS"); dir && target.width >= 256) {
                static u32 n = 0;
                char name[512];
                std::snprintf(name, sizeof name, "%s/target_%04u_%08X.ppm", dir, n++, target.color_va);
                if (std::FILE* f = std::fopen(name, "wb")) {
                    std::fprintf(f, "P6\n%u %u\n255\n", target.width, target.height);
                    for (u32 y = target.height; y-- > 0;)  // top row first in the image file
                        for (u32 x = 0; x < target.width; ++x) std::fwrite(xfer.ptr + (y * target.width + x) * 4, 1, 3, f);
                    std::fclose(f);
                }
            }
            write_guest_color(mem, xfer.ptr);
            ++stats.flushes;
        }
        drop_target();
    }

    bool bind_target(Memory& mem, const std::array<u32, pica::reg::COUNT>& regs) {
        const u32 color_va = Memory::pa_to_va(regs[COLOR_ADDR] << 3);
        const u32 depth_va = Memory::pa_to_va(regs[DEPTH_ADDR] << 3);
        const u32 w = regs[FB_DIM] & 0x7FF, h = (regs[FB_DIM] >> 12 & 0x3FF) + 1;
        const u32 fmt = regs[COLOR_FORMAT] >> 16 & 7;
        if (!color_va || !w || fmt > 4) return false;
        if (target.color.img && target.color_va == color_va && target.depth_va == depth_va && target.width == w &&
            target.height == h && target.format == fmt)
            return true;
        flush(mem);
        target.color_va = color_va, target.depth_va = depth_va, target.width = w, target.height = h;
        target.format = fmt;
        target.color = make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        target.depth = make_image(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageView views[2] = {target.color.view, target.depth.view};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = render_pass;
        fi.attachmentCount = 2;
        fi.pAttachments = views;
        fi.width = w, fi.height = h, fi.layers = 1;
        vkCreateFramebuffer(dev, &fi, nullptr, &target.fb);
        target.fresh = true;
        return true;
    }

    // Decodes and (re)uploads a texture unit if its contents changed. Records into cmd.
    Texture& texture(const Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, u32 unit, VkDeviceSize& xfer_off) {
        const u32* r = TEX_REGS[unit];
        const u32 h = regs[r[0]] & 0x7FF, w = regs[r[0]] >> 16 & 0x7FF;
        const auto fmt = pica::TexFormat(regs[r[3]] & 0xF);
        const u32 va = Memory::pa_to_va(regs[r[2]] << 3);
        const size_t size = pica::texture_size(fmt, w, h);
        if (!va || !w || !h || !size || w % 8 || h % 8) return dummy;
        const u64 key = fnv(&va, 4, fnv(&w, 4, fnv(&h, 4, u64(fmt))));
        Texture& t = textures[key];
        const u64 hash = fnv(mem.host(va), size);
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
        for (u32 row = 0; row < h; ++row)
            std::memcpy(xfer.ptr + xfer_off + size_t(row) * w * 4, rgba.data() + size_t(h - 1 - row) * w, w * 4);
        upload_image(t.image.img, w, h, xfer_off);
        xfer_off += (rgba.size() * 4 + 255) & ~VkDeviceSize(255);
        t.hash = hash, t.ready = true;
        return t;
    }

    VkPipeline pipeline(VkShaderModule vs, VkShaderModule fs, const std::array<u32, pica::reg::COUNT>& regs) {
        const u32 topo = regs[PRIMITIVE_CONFIG] >> 8 & 3;
        const u32 state[] = {topo, regs[CULL] & 3, regs[COLOR_OP], regs[BLEND_FUNC], regs[DEPTH_COLOR_MASK],
                             regs[COLOR_WRITE_ENABLE] != 0, regs[DEPTH_WRITE_ENABLE] != 0};
        const u64 key = fnv(state, sizeof state, fnv(&vs, sizeof vs, fnv(&fs, sizeof fs)));
        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs,
                     "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs,
                     "main", nullptr};
        VkVertexInputBindingDescription binding{0, sizeof(ShadedVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[6];
        for (u32 i = 0; i < 6; ++i) attrs[i] = {i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 6, vi.pVertexAttributeDescriptions = attrs;
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
                                      VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 3, dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.stageCount = 2, pi.pStages = stages;
        pi.pVertexInputState = &vi, pi.pInputAssemblyState = &ia, pi.pViewportState = &vp;
        pi.pRasterizationState = &rs, pi.pMultisampleState = &ms, pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cbs, pi.pDynamicState = &dy;
        pi.layout = pipeline_layout, pi.renderPass = render_pass;
        VkPipeline p = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p), "vkCreateGraphicsPipelines");
        pipelines[key] = p;
        ++stats.pipelines;
        return p;
    }

    void draw(Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs, bool indexed) {
        auto skip = [&](const char* why) {
            static std::map<std::string, bool> seen;
            if (!seen[why]) std::fprintf(stderr, "renderer: skipping draws: %s\n", why), seen[why] = true;
            ++stats.skipped;
        };
        if (!bind_target(mem, regs)) return skip("unsupported or missing colour buffer");

        std::vector<pica::VertexInput> verts;
        if (!pica::load_vertices(mem, regs, vs, indexed, verts) || verts.empty()) return skip("vertex fetch");

        // Vertex shader: run on the CPU, outputs packed through VS_OUTMAP.
        const u32 entry = regs[VS_ENTRY] & 0xFFFF;
        std::vector<ShadedVertex> shaded(verts.size());
        {
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
        const VkShaderModule vmod = pass_vs;
        const VkShaderModule fmod = module(pica::generate_fragment_shader(regs), ShaderStage::Fragment);
        if (!vmod || !fmod) return skip("shader compile");
        const VkPipeline pipe = pipeline(vmod, fmod, regs);
        if (!pipe) return skip("pipeline creation");

        // Vertex data and uniforms.
        const VkDeviceSize vbytes = shaded.size() * sizeof(ShadedVertex);
        ensure(vbuf, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        std::memcpy(vbuf.ptr, shaded.data(), vbytes);
        u8* u = ubuf.ptr;
        std::memcpy(u, vs.float_uniforms.data(), 96 * 16);
        for (u32 i = 0; i < 4; ++i) {
            const u32 iv = vs.int_uniforms[i];
            const s32 v[4] = {s32(iv & 0xFF), s32(iv >> 8 & 0xFF), s32(s8(iv >> 16)), 0};
            std::memcpy(u + 1536 + i * 16, v, 16);
        }
        const float depth[4] = {f24_to_f32(regs[DEPTH_SCALE] & 0xFFFFFF), f24_to_f32(regs[DEPTH_OFFSET] & 0xFFFFFF), 0, 0};
        std::memcpy(u + 1600, depth, 16);
        const u32 bools = vs.bool_uniforms;
        std::memcpy(u + 1616, &bools, 4);
        std::memcpy(lbuf.ptr, &regs[LIGHT_REGS], LIGHT_REG_WORDS * 4);
        std::memcpy(lbuf.ptr + LIGHT_REG_WORDS * 4, vs.light_lut.data(), sizeof(vs.light_lut));

        begin();
        // Texture uploads (before the render pass).
        ensure(xfer, 16u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkDeviceSize xoff = 0;
        Texture* tex[3];
        for (u32 i = 0; i < 3; ++i)
            tex[i] = regs[TEX_CONFIG] >> i & 1 ? &texture(mem, regs, i, xoff) : &dummy;
        if (target.fresh) {  // bring in the guest's current pixels
            const VkDeviceSize bytes = VkDeviceSize(target.width) * target.height * 4;
            read_guest_color(mem, xfer.ptr + xoff);
            barrier(target.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.bufferOffset = xoff;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {target.width, target.height, 1};
            vkCmdCopyBufferToImage(cmd, xfer.buf, target.color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(target.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            barrier(target.depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            xoff += bytes;
        }

        // Descriptors.
        vkResetDescriptorPool(dev, desc_pool, 0);
        VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = desc_pool;
        dai.descriptorSetCount = 2;
        dai.pSetLayouts = set_layouts;
        vkAllocateDescriptorSets(dev, &dai, sets);
        const VkDescriptorBufferInfo bi[2] = {{ubuf.buf, 0, UBO_SIZE}, {lbuf.buf, 0, LIGHT_BUF_SIZE}};
        VkDescriptorImageInfo ii[3];
        for (u32 i = 0; i < 3; ++i)
            ii[i] = {sampler(regs[TEX_REGS[i][1]]), tex[i]->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
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
        rbi.framebuffer = target.fb;
        rbi.renderArea = {{0, 0}, {target.width, target.height}};
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        if (target.fresh) {  // depth starts at the guest's last fill value for this buffer
            auto it = depth_clears.find(target.depth_va);
            VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT, 1, {}};
            ca.clearValue.depthStencil = {it == depth_clears.end() ? 0.0f : it->second, 0};
            VkClearRect rect{{{0, 0}, {target.width, target.height}}, 0, 1};
            vkCmdClearAttachments(cmd, 1, &ca, 1, &rect);
            target.fresh = false;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        const float vw = f24_to_f32(regs[VIEWPORT_W] & 0xFFFFFF) * 2, vh = f24_to_f32(regs[VIEWPORT_H] & 0xFFFFFF) * 2;
        VkViewport viewport{float(regs[VIEWPORT_XY] & 0x3FF), float(regs[VIEWPORT_XY] >> 16 & 0x3FF), vw, vh, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {target.width, target.height}};
        if ((regs[SCISSOR_MODE] & 3) == 3) {
            const s32 x1 = regs[SCISSOR_MIN] & 0x3FF, y1 = regs[SCISSOR_MIN] >> 16 & 0x3FF;
            const s32 x2 = regs[SCISSOR_MAX] & 0x3FF, y2 = regs[SCISSOR_MAX] >> 16 & 0x3FF;
            scissor = {{x1, y1}, {u32(std::max(0, x2 - x1 + 1)), u32(std::max(0, y2 - y1 + 1))}};
        }
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        const u32 bc = regs[BLEND_COLOR];
        const float consts[4] = {(bc & 0xFF) / 255.f, (bc >> 8 & 0xFF) / 255.f, (bc >> 16 & 0xFF) / 255.f, (bc >> 24) / 255.f};
        vkCmdSetBlendConstants(cmd, consts);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, sets, 0, nullptr);
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf.buf, &zero);
        vkCmdDraw(cmd, u32(verts.size()), 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        const auto t0 = std::chrono::steady_clock::now();
        submit_and_wait();
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 50 && std::getenv("DOT_TRACE_DRAWS"))
            std::fprintf(stderr, "renderer: slow draw %.0f ms (%zu vertices, entry %u)\n", ms, verts.size(), entry);
        ++stats.draws;
        if (std::getenv("DOT_TRACE_FB"))
            std::fprintf(stderr, "renderer:   draw %zu vertices into %08X\n", verts.size(), target.color_va);
    }
};

Renderer::Renderer(Context& ctx) : impl_(std::make_unique<Impl>(ctx)) {}
Renderer::~Renderer() = default;
bool Renderer::init() { return impl_->init(); }

template <class F> static void timed(const char* what, F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 30 && std::getenv("DOT_TRACE_DRAWS")) std::fprintf(stderr, "renderer: slow %s %.0f ms\n", what, ms);
}

void Renderer::draw(core::Memory& mem, const std::array<u32, pica::reg::COUNT>& regs, const pica::ShaderState& vs,
                    bool indexed) {
    std::lock_guard lk(impl_->lock);
    timed("draw (total)", [&] { impl_->draw(mem, regs, vs, indexed); });
}

void Renderer::flush(core::Memory& mem) {
    std::lock_guard lk(impl_->lock);
    timed("flush", [&] { impl_->flush(mem); });
}

void Renderer::memory_fill(u32 start, u32, u32 value, u32 width_mode) {
    std::lock_guard lk(impl_->lock);
    impl_->depth_clears[start] = width_mode == 0 ? (value & 0xFFFF) / 65535.0f : (value & 0xFFFFFF) / 16777215.0f;
}

Renderer::Stats Renderer::stats() const { return impl_->stats; }

}  // namespace gfx::vulkan
