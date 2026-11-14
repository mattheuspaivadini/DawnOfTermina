#include "gfx/pica/command_processor.h"

#include <bit>

namespace gfx::pica {

float f24_to_f32(u32 v) {
    // 1 sign, 7 exponent (bias 63), 16 mantissa -> IEEE single (bias 127).
    const u32 sign = v >> 23 & 1, exp = v >> 16 & 0x7F, mant = v & 0xFFFF;
    u32 bits = sign << 31;
    if (exp == 0x7F) bits |= 0xFFu << 23 | mant << 7;
    else if (exp) bits |= (exp + 64) << 23 | mant << 7;  // exp 0: flush to signed zero
    return std::bit_cast<float>(bits);
}

bool CommandProcessor::process(std::span<const u32> words) {
    for (int hops = 0; hops < 4096; ++hops) {  // command buffer jumps chain lists together
        size_t i = 0;
        jump_channel_ = -1;
        while (i + 2 <= words.size() && jump_channel_ < 0) {
            const u32 value = words[i], header = words[i + 1];
            const u32 id = header & 0xFFFF, mask = header >> 16 & 0xF;
            const u32 extra = header >> 20 & 0x7FF;
            const bool consecutive = header >> 31;
            if (i + 2 + extra > words.size()) return false;
            write(id, value, mask);
            for (u32 n = 0; n < extra && jump_channel_ < 0; ++n)
                write(consecutive ? id + n + 1 : id, words[i + 2 + n], mask);
            i += 2 + extra + (extra & 1);  // commands are padded to 8 bytes
        }
        if (jump_channel_ < 0) return i >= words.size();
        // Continue in the buffer the jump register points at.
        const u32 c = u32(jump_channel_);
        const u32 pa = regs_[reg::CMDBUF_ADDR0 + c] << 3, bytes = (regs_[reg::CMDBUF_SIZE0 + c] & 0x1FFFFF) << 3;
        if (!fetch || !bytes) return false;
        words = fetch(pa, bytes);
        if (words.empty()) return false;
    }
    return false;
}

void CommandProcessor::write(u32 id, u32 value, u32 byte_mask) {
    if (id >= reg::COUNT) return;
    u32 m = 0;
    for (u32 b = 0; b < 4; ++b)
        if (byte_mask >> b & 1) m |= 0xFFu << (b * 8);
    u32& r = regs_[id];
    r = (r & ~m) | (value & m);
    value = r;

    using namespace reg;
    if (id == CMDBUF_JUMP0 || id == CMDBUF_JUMP0 + 1) {
        jump_channel_ = int(id - CMDBUF_JUMP0);
    } else if (id == DRAW_ARRAYS || id == DRAW_ELEMENTS) {
        if (on_draw) on_draw(id);
    } else if (id == VS_BOOL_UNIFORMS) {
        vs_.bool_uniforms = u16(value);
    } else if (id >= VS_INT_UNIFORMS && id < VS_INT_UNIFORMS + 4) {
        vs_.int_uniforms[id - VS_INT_UNIFORMS] = value;
    } else if (id == VS_FLOAT_UNIFORM_SETUP) {
        uniform_words_ = 0;
    } else if (id >= VS_FLOAT_UNIFORM_DATA && id < VS_FLOAT_UNIFORM_DATA + 8) {
        const u32 setup = regs_[VS_FLOAT_UNIFORM_SETUP];
        const bool f32 = setup >> 31;
        uniform_buf_[uniform_words_++] = value;
        if (uniform_words_ < (f32 ? 4u : 3u)) return;
        uniform_words_ = 0;
        const u32 index = setup & 0xFF;
        Vec4 v;
        const u32* b = uniform_buf_;
        if (f32) {  // written w, z, y, x
            v = {std::bit_cast<float>(b[3]), std::bit_cast<float>(b[2]), std::bit_cast<float>(b[1]),
                 std::bit_cast<float>(b[0])};
        } else {  // four 24-bit floats packed big-end first: w, z, y, x
            v.w = f24_to_f32(b[0] >> 8);
            v.z = f24_to_f32((b[0] & 0xFF) << 16 | b[1] >> 16);
            v.y = f24_to_f32((b[1] & 0xFFFF) << 8 | b[2] >> 24);
            v.x = f24_to_f32(b[2] & 0xFFFFFF);
        }
        if (index < vs_.float_uniforms.size()) vs_.float_uniforms[index] = v;
        regs_[VS_FLOAT_UNIFORM_SETUP] = (setup & ~0xFFu) | ((index + 1) & 0xFF);
    } else if (id == FIXED_ATTRIB_INDEX) {
        fixed_words_ = 0;
    } else if (id >= FIXED_ATTRIB_DATA && id < FIXED_ATTRIB_DATA + 3) {
        fixed_buf_[fixed_words_++] = value;
        if (fixed_words_ < 3) return;
        fixed_words_ = 0;
        u32& index = regs_[FIXED_ATTRIB_INDEX];
        const u32* b = fixed_buf_;
        const Vec4 v{f24_to_f32(b[2] & 0xFFFFFF), f24_to_f32((b[1] & 0xFFFF) << 8 | b[2] >> 24),
                     f24_to_f32((b[0] & 0xFF) << 16 | b[1] >> 16), f24_to_f32(b[0] >> 8)};
        if ((index & 0xF) < 12) vs_.fixed_attribs[index & 0xF] = v;
        index = (index & ~0xFu) | ((index + 1) & 0xF);
    } else if (id >= LIGHTING_LUT_DATA && id < LIGHTING_LUT_DATA + 8) {
        u32& sel = regs_[LIGHTING_LUT_INDEX];
        if (const u32 table = sel >> 8 & 0x1F; table < 24) vs_.light_lut[table * 256 + (sel & 0xFF)] = value;
        sel = (sel & ~0xFFu) | ((sel + 1) & 0xFF);
    } else if (id >= FOG_LUT_DATA && id < FOG_LUT_DATA + 8) {
        u32& index = regs_[FOG_LUT_INDEX];
        vs_.fog_lut[index & 0x7F] = value;
        index = (index & ~0x7Fu) | ((index + 1) & 0x7F);
    } else if (id >= VS_PROGRAM_DATA && id < VS_PROGRAM_DATA + 8) {
        u32& off = regs_[VS_PROGRAM_OFFSET];
        if (off < vs_.code.size()) vs_.code[off] = value;
        ++off;
    } else if (id >= VS_SWIZZLE_DATA && id < VS_SWIZZLE_DATA + 8) {
        u32& off = regs_[VS_SWIZZLE_OFFSET];
        if (off < vs_.swizzle.size()) vs_.swizzle[off] = value;
        ++off;
    }
}

void CommandProcessor::save_state(core::StateWriter& w) const {
    w.section("PICA");
    w.put(regs_), w.put(vs_), w.put(uniform_buf_), w.put(uniform_words_), w.put(fixed_buf_), w.put(fixed_words_);
}

void CommandProcessor::load_state(core::StateReader& r) {
    r.section("PICA");
    r.get(regs_), r.get(vs_), r.get(uniform_buf_), r.get(uniform_words_), r.get(fixed_buf_), r.get(fixed_words_);
}

}  // namespace gfx::pica
