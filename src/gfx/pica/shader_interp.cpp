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

#include "gfx/pica/shader_interp.h"

#include <algorithm>
#include <cmath>

namespace gfx::pica {

namespace {

u32 bits(u32 v, u32 lo, u32 n) { return v >> lo & ((1u << n) - 1); }

using V = Vec4;

float pmul(float a, float b) {  // PICA: 0 * inf = 0
    const float p = a * b;
    return std::isnan(p) && !std::isnan(a) && !std::isnan(b) ? 0.0f : p;
}

struct State {
    const ShaderState& s;
    V v[16], r[16], o[16];
    int a0x = 0, a0y = 0, aL = 0;
    bool ccx = false, ccy = false;

    V src(u32 reg, u32 idx, u32 swz, bool neg) const {
        V base;
        if (reg < 0x10) base = v[reg];
        else if (reg < 0x20) base = r[reg - 0x10];
        else {
            const int off = idx == 1 ? a0x : idx == 2 ? a0y : idx == 3 ? aL : 0;
            base = s.float_uniforms[std::clamp(int(reg - 0x20) + off, 0, 95)];
        }
        const float* c = &base.x;
        V out{c[swz >> 6 & 3], c[swz >> 4 & 3], c[swz >> 2 & 3], c[swz & 3]};
        if (neg) out = {-out.x, -out.y, -out.z, -out.w};
        return out;
    }

    void dst(u32 reg, u32 desc, const V& val) {
        V& d = reg < 0x10 ? o[reg] : r[reg - 0x10];
        if (desc >> 3 & 1) d.x = val.x;
        if (desc >> 2 & 1) d.y = val.y;
        if (desc >> 1 & 1) d.z = val.z;
        if (desc & 1) d.w = val.w;
    }

    bool cond(u32 w) const {
        const bool x = bits(w, 25, 1) == ccx, y = bits(w, 24, 1) == ccy;
        switch (bits(w, 22, 2)) {
        case 0: return x || y;
        case 1: return x && y;
        case 2: return x;
        default: return y;
        }
    }
};

V splat(float f) { return {f, f, f, f}; }
float dot3(const V& a, const V& b) { return pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z); }
float dot4(const V& a, const V& b) { return dot3(a, b) + pmul(a.w, b.w); }

}  // namespace

void run_vertex_shader(const ShaderState& shader, u32 entry, const Vec4 in[16], Vec4 out[16]) {
    State st{shader, {}, {}, {}};
    for (int i = 0; i < 16; ++i) st.v[i] = in[i], st.r[i] = {}, st.o[i] = {};
    struct Frame {
        u32 final, ret, loop;
        int rep, inc;
    };
    Frame stack[16];
    int sp = 0;
    u32 pc = entry;
    for (int guard = 0; guard < 65536 && pc < 512; ++guard) {
        while (sp > 0 && pc == stack[sp - 1].final) {
            Frame& f = stack[sp - 1];
            st.aL += f.inc;
            if (f.rep-- == 0) pc = f.ret, --sp;
            else pc = f.loop;
        }
        const u32 w = shader.code[pc], op = w >> 26;
        const u32 dstaddr = bits(w, 10, 12), num = bits(w, 0, 8);
        auto push = [&](u32 final, u32 ret, int rep, int inc, u32 loop) {
            if (sp < 16) stack[sp++] = {final, ret, loop, rep, inc};
        };
        ++pc;
        if (op >= 0x30) {  // MAD / MADI
            const bool inv = op < 0x38;
            const u32 desc = shader.swizzle[bits(w, 0, 5)];
            const u32 idx = bits(w, 22, 2);
            const V a = st.src(bits(w, 17, 5), 0, bits(desc, 5, 8), bits(desc, 4, 1));
            const V b = st.src(inv ? bits(w, 12, 5) : bits(w, 10, 7), inv ? 0 : idx, bits(desc, 14, 8), bits(desc, 13, 1));
            const V c = st.src(inv ? bits(w, 5, 7) : bits(w, 5, 5), inv ? idx : 0, bits(desc, 23, 8), bits(desc, 22, 1));
            st.dst(bits(w, 24, 5), desc,
                   {pmul(a.x, b.x) + c.x, pmul(a.y, b.y) + c.y, pmul(a.z, b.z) + c.z, pmul(a.w, b.w) + c.w});
            continue;
        }
        switch (op) {
        case 0x21: continue;  // NOP
        case 0x22:            // END
        case 0x2A: case 0x2B: case 0x07:  // EMIT, SETEMIT, LITP: unsupported in a vertex shader
            goto done;
        case 0x20:  // BREAK
            if (sp > 0) pc = stack[--sp].ret;
            else goto done;
            continue;
        case 0x23:  // BREAKC
            if (st.cond(w)) {
                if (sp > 0) pc = stack[--sp].ret;
                else goto done;
            }
            continue;
        case 0x24: case 0x25: case 0x26: {  // CALL, CALLC, CALLU
            const bool c = op == 0x24 ? true : op == 0x25 ? st.cond(w) : (shader.bool_uniforms >> bits(w, 22, 4) & 1);
            if (c) push(dstaddr + num, pc, 0, 0, 0), pc = dstaddr;
            continue;
        }
        case 0x27: case 0x28: {  // IFU, IFC
            const bool c = op == 0x28 ? st.cond(w) : (shader.bool_uniforms >> bits(w, 22, 4) & 1);
            if (c) push(dstaddr, dstaddr + num, 0, 0, 0);
            else push(dstaddr + num, dstaddr + num, 0, 0, 0), pc = dstaddr;
            continue;
        }
        case 0x29: {  // LOOP
            const u32 iu = shader.int_uniforms[bits(w, 22, 2)];
            st.aL = int(iu >> 8 & 0xFF);
            push(dstaddr + 1, dstaddr + 1, int(iu & 0xFF), int(s8(iu >> 16)), pc);
            continue;
        }
        case 0x2C:  // JMPC
            if (st.cond(w)) pc = dstaddr;
            continue;
        case 0x2D:  // JMPU
            if (bool(shader.bool_uniforms >> bits(w, 22, 4) & 1) != bool(num & 1)) pc = dstaddr;
            continue;
        default: break;
        }
        // Formats 1 / 1i / 1c
        const bool inv = op == 0x18 || op == 0x19 || op == 0x1A || op == 0x1B;
        const u32 desc = shader.swizzle[bits(w, 0, 7)];
        const u32 idx = bits(w, 19, 2);
        const V a = st.src(inv ? bits(w, 14, 5) : bits(w, 12, 7), inv ? 0 : idx, bits(desc, 5, 8), bits(desc, 4, 1));
        const V b = st.src(inv ? bits(w, 7, 7) : bits(w, 7, 5), inv ? idx : 0, bits(desc, 14, 8), bits(desc, 13, 1));
        const u32 d = bits(w, 21, 5);
        if ((op & 0x3E) == 0x2E) {  // CMP
            auto cmp = [](u32 f, float x, float y) {
                switch (f) {
                case 0: return x == y;
                case 1: return x != y;
                case 2: return x < y;
                case 3: return x <= y;
                case 4: return x > y;
                case 5: return x >= y;
                default: return false;
                }
            };
            st.ccx = cmp(bits(w, 24, 3), a.x, b.x);
            st.ccy = cmp(bits(w, 21, 3), a.y, b.y);
            continue;
        }
        auto per = [&](auto f) { return V{f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w)}; };
        switch (op) {
        case 0x00: st.dst(d, desc, per([](float x, float y) { return x + y; })); break;
        case 0x08: st.dst(d, desc, per(pmul)); break;
        case 0x01: st.dst(d, desc, splat(dot3(a, b))); break;
        case 0x02: st.dst(d, desc, splat(dot4(a, b))); break;
        case 0x03: case 0x18: st.dst(d, desc, splat(dot3(a, b) + b.w)); break;  // DPH: (a.xyz, 1) . b
        case 0x04: case 0x19: st.dst(d, desc, {1.0f, pmul(a.y, b.y), a.z, b.w}); break;
        case 0x05: st.dst(d, desc, splat(std::exp2(a.x))); break;
        case 0x06: st.dst(d, desc, splat(std::log2(a.x))); break;
        case 0x09: case 0x1A: st.dst(d, desc, per([](float x, float y) { return x >= y ? 1.0f : 0.0f; })); break;
        case 0x0A: case 0x1B: st.dst(d, desc, per([](float x, float y) { return x < y ? 1.0f : 0.0f; })); break;
        case 0x0B: st.dst(d, desc, {std::floor(a.x), std::floor(a.y), std::floor(a.z), std::floor(a.w)}); break;
        case 0x0C: st.dst(d, desc, per([](float x, float y) { return x > y ? x : y; })); break;
        case 0x0D: st.dst(d, desc, per([](float x, float y) { return x < y ? x : y; })); break;
        case 0x0E: st.dst(d, desc, splat(1.0f / a.x)); break;
        case 0x0F: st.dst(d, desc, splat(1.0f / std::sqrt(a.x))); break;
        case 0x12:  // MOVA
            if (desc >> 3 & 1) st.a0x = int(a.x);
            if (desc >> 2 & 1) st.a0y = int(a.y);
            break;
        case 0x13: st.dst(d, desc, a); break;
        default: goto done;  // unknown opcode: stop rather than run garbage
        }
    }
done:
    for (int i = 0; i < 16; ++i) out[i] = st.o[i];
}

}  // namespace gfx::pica
