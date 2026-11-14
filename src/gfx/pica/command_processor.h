#pragma once

#include <array>
#include <functional>
#include <span>

#include "core/types.h"

namespace gfx::pica {

// Register ids (word index into the 0x300-word PICA200 register file) that act as triggers.
namespace reg {
constexpr u32 COUNT = 0x300;
constexpr u32 FIXED_ATTRIB_INDEX = 0x232;
constexpr u32 FIXED_ATTRIB_DATA = 0x233;  // 3 regs: four packed float24
constexpr u32 CMDBUF_SIZE0 = 0x238, CMDBUF_ADDR0 = 0x23A, CMDBUF_JUMP0 = 0x23C;  // channel 1 at +1
constexpr u32 LIGHTING_LUT_INDEX = 0x1C5;  // bits 0-7 entry, 8-12 table
constexpr u32 LIGHTING_LUT_DATA = 0x1C8;   // 8 regs, all write the same FIFO
constexpr u32 DRAW_ARRAYS = 0x22E;
constexpr u32 DRAW_ELEMENTS = 0x22F;
constexpr u32 VS_BOOL_UNIFORMS = 0x2B0;
constexpr u32 VS_INT_UNIFORMS = 0x2B1;  // 4 regs
constexpr u32 VS_FLOAT_UNIFORM_SETUP = 0x2C0;
constexpr u32 VS_FLOAT_UNIFORM_DATA = 0x2C1;  // 8 regs, all write the same FIFO
constexpr u32 VS_PROGRAM_OFFSET = 0x2CB;
constexpr u32 VS_PROGRAM_DATA = 0x2CC;  // 8 regs
constexpr u32 VS_SWIZZLE_OFFSET = 0x2D5;
constexpr u32 VS_SWIZZLE_DATA = 0x2D6;  // 8 regs
}  // namespace reg

struct Vec4 {
    float x, y, z, w;
};

float f24_to_f32(u32 v);

struct ShaderState {
    std::array<u32, 512> code{};
    std::array<u32, 128> swizzle{};
    std::array<Vec4, 96> float_uniforms{};
    std::array<u32, 4> int_uniforms{};
    u16 bool_uniforms = 0;
    std::array<Vec4, 16> fixed_attribs{};  // default values of vertex attributes
    // Fragment lighting tables (not shader state proper, but uploaded the same way): 24 tables
    // of 256 raw entries {bits 0-11 value 0.12, bits 12-23 signed difference 1.11}.
    // Tables: 0 D0, 1 D1, 3 FR, 4 RB, 5 RG, 6 RR, 8-15 SP per light, 16-23 DA per light.
    std::array<u32, 24 * 256> light_lut{};
};

// Executes PICA200 command lists (as submitted through GSP) into register state.
// Rasterisation is delegated to `on_draw`, called with the trigger register id.
// shortcut: geometry shader (0x280-0x2AF) uploads and immediate-mode vertices (fixed
// attribute index 0xF) are stored as plain registers only; add when the game uses them.
class CommandProcessor {
public:
    std::function<void(u32 trigger_reg)> on_draw;
    // Resolves a command buffer jump target (physical address, size in bytes) to words.
    std::function<std::span<const u32>(u32 pa, u32 bytes)> fetch;

    // `words` must be 8-byte aligned command list data. Returns false on a truncated list.
    bool process(std::span<const u32> words);
    void write(u32 id, u32 value, u32 byte_mask = 0xF);

    const std::array<u32, reg::COUNT>& regs() const { return regs_; }
    const ShaderState& vs() const { return vs_; }

private:
    std::array<u32, reg::COUNT> regs_{};
    ShaderState vs_;
    u32 uniform_buf_[4]{};
    u32 uniform_words_ = 0;
    u32 fixed_buf_[3]{};
    u32 fixed_words_ = 0;
    int jump_channel_ = -1;  // set by a write to CMDBUF_JUMP0/1
};

}  // namespace gfx::pica
