#pragma once

#include <array>
#include <functional>
#include <span>

#include "core/types.h"

namespace gfx::pica {

// Register ids (word index into the 0x300-word PICA200 register file) that act as triggers.
namespace reg {
constexpr u32 COUNT = 0x300;
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
};

// Executes PICA200 command lists (as submitted through GSP) into register state.
// Rasterisation is delegated to `on_draw`, called with the trigger register id.
// shortcut: geometry shader (0x280-0x2AF) and fixed-attribute (0x232) uploads are
// stored as plain registers only; add trigger handling when the game uses them.
class CommandProcessor {
public:
    std::function<void(u32 trigger_reg)> on_draw;

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
};

}  // namespace gfx::pica
