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

#include "gfx/pica/shader_gen.h"

#include <cstdio>
#include <format>
#include <optional>
#include <set>
#include <vector>

namespace gfx::pica {

namespace {

enum Op : u32 {
    ADD = 0x00, DP3 = 0x01, DP4 = 0x02, DPH = 0x03, DST = 0x04, EX2 = 0x05, LG2 = 0x06, LITP = 0x07,
    MUL = 0x08, SGE = 0x09, SLT = 0x0A, FLR = 0x0B, MAX = 0x0C, MIN = 0x0D, RCP = 0x0E, RSQ = 0x0F,
    MOVA = 0x12, MOV = 0x13, DPHI = 0x18, DSTI = 0x19, SGEI = 0x1A, SLTI = 0x1B,
    BREAK = 0x20, NOP = 0x21, END = 0x22, BREAKC = 0x23, CALL = 0x24, CALLC = 0x25, CALLU = 0x26,
    IFU = 0x27, IFC = 0x28, LOOP = 0x29, EMIT = 0x2A, SETEMIT = 0x2B, JMPC = 0x2C, JMPU = 0x2D,
};

u32 bits(u32 v, u32 lo, u32 n) { return v >> lo & ((1u << n) - 1); }

struct Translator {
    const VsSource& s;
    std::string body;
    std::set<u32> inputs_read;

    std::string swz(u32 field) const {
        std::string out;
        for (int i = 0; i < 4; ++i) out += "xyzw"[field >> (6 - 2 * i) & 3];
        return out;
    }

    // `reg` is the 5- or 7-bit source field; relative addressing only applies to uniforms.
    std::string src(u32 reg, u32 idx, u32 swizzle_field, bool negate) {
        std::string base;
        if (reg < 0x10) {
            base = std::format("v[{}]", reg);
            inputs_read.insert(reg);
        } else if (reg < 0x20) {
            base = std::format("r[{}]", reg - 0x10);
        } else {
            static const char* rel[] = {"", " + a0.x", " + a0.y", " + aL"};
            base = idx ? std::format("u.c[clamp({}{}, 0, 95)]", reg - 0x20, rel[idx])
                       : std::format("u.c[{}]", reg - 0x20);
        }
        return std::format("{}{}.{}", negate ? "-" : "", base, swz(swizzle_field));
    }

    std::string dest(u32 reg) const {
        return reg < 0x10 ? std::format("o[{}]", reg) : std::format("r[{}]", reg - 0x10);
    }

    static std::string mask_str(u32 desc) {
        std::string m;
        for (int i = 0; i < 4; ++i)
            if (desc >> (3 - i) & 1) m += "xyzw"[i];
        return m;
    }

    void assign(u32 dst, u32 desc, const std::string& value) {
        const std::string m = mask_str(desc);
        if (!m.empty()) body += std::format("        {}.{} = ({}).{};\n", dest(dst), m, value, m);
    }

    std::string cond(u32 w) const {
        const char* x = bits(w, 25, 1) ? "cc.x" : "!cc.x";
        const char* y = bits(w, 24, 1) ? "cc.y" : "!cc.y";
        switch (bits(w, 22, 2)) {
        case 0: return std::format("({} || {})", x, y);
        case 1: return std::format("({} && {})", x, y);
        case 2: return x;
        default: return y;
        }
    }

    // Arithmetic instruction (formats 1, 1i, 1c, 5, 5i). Returns false if unsupported.
    bool arith(u32 w, std::string* error) {
        const u32 op = w >> 26;
        if (op >= 0x30) {  // MAD / MADI
            const bool inv = op < 0x38;
            const u32 desc = s.swizzles[bits(w, 0, 5)];
            const u32 idx = bits(w, 22, 2);
            const u32 r1 = bits(w, 17, 5);
            const u32 r2 = inv ? bits(w, 12, 5) : bits(w, 10, 7);
            const u32 r3 = inv ? bits(w, 5, 7) : bits(w, 5, 5);
            const std::string a = src(r1, 0, bits(desc, 5, 8), bits(desc, 4, 1));
            const std::string b = src(r2, inv ? 0 : idx, bits(desc, 14, 8), bits(desc, 13, 1));
            const std::string c = src(r3, inv ? idx : 0, bits(desc, 23, 8), bits(desc, 22, 1));
            assign(bits(w, 24, 5), desc, std::format("sanitize_mul({}, {}) + {}", a, b, c));
            return true;
        }
        const bool inv = op == DPHI || op == DSTI || op == SGEI || op == SLTI;
        const u32 desc = s.swizzles[bits(w, 0, 7)];
        const u32 idx = bits(w, 19, 2);
        const u32 r1 = inv ? bits(w, 14, 5) : bits(w, 12, 7);
        const u32 r2 = inv ? bits(w, 7, 7) : bits(w, 7, 5);
        const std::string a = src(r1, inv ? 0 : idx, bits(desc, 5, 8), bits(desc, 4, 1));
        const std::string b = src(r2, inv ? idx : 0, bits(desc, 14, 8), bits(desc, 13, 1));
        const u32 d = bits(w, 21, 5);

        if ((op & 0x3E) == 0x2E) {  // CMP: x/y compared with independent operators
            static const char* cmp[] = {"==", "!=", "<", "<=", ">", ">="};
            const u32 cx = bits(w, 24, 3), cy = bits(w, 21, 3);
            if (cx > 5 || cy > 5) return *error = "bad CMP operator", false;
            body += std::format("        {{ vec4 ca = {}; vec4 cb = {}; cc = bvec2(ca.x {} cb.x, ca.y {} cb.y); }}\n",
                                a, b, cmp[cx], cmp[cy]);
            return true;
        }
        switch (op) {
        case ADD: assign(d, desc, std::format("{} + {}", a, b)); break;
        case MUL: assign(d, desc, std::format("sanitize_mul({}, {})", a, b)); break;
        case DP3: assign(d, desc, std::format("vec4(dot3_s({}, {}))", a, b)); break;
        case DP4: assign(d, desc, std::format("vec4(dot4_s({}, {}))", a, b)); break;
        case DPH: case DPHI: assign(d, desc, std::format("vec4(dot4_s(vec4(({}).xyz, 1.0), {}))", a, b)); break;
        case DST: case DSTI: assign(d, desc, std::format("dst_op({}, {})", a, b)); break;
        case EX2: assign(d, desc, std::format("vec4(exp2(({}).x))", a)); break;
        case LG2: assign(d, desc, std::format("vec4(log2(({}).x))", a)); break;
        case SGE: case SGEI: assign(d, desc, std::format("vec4(greaterThanEqual({}, {}))", a, b)); break;
        case SLT: case SLTI: assign(d, desc, std::format("vec4(lessThan({}, {}))", a, b)); break;
        case FLR: assign(d, desc, std::format("floor({})", a)); break;
        case MAX: assign(d, desc, std::format("max_op({}, {})", a, b)); break;
        case MIN: assign(d, desc, std::format("min_op({}, {})", a, b)); break;
        case RCP: assign(d, desc, std::format("vec4(1.0 / ({}).x)", a)); break;
        case RSQ: assign(d, desc, std::format("vec4(inversesqrt(({}).x))", a)); break;
        case MOV: assign(d, desc, a); break;
        case MOVA:
            if (desc >> 3 & 1) body += std::format("        a0.x = int(({}).x);\n", a);
            if (desc >> 2 & 1) body += std::format("        a0.y = int(({}).y);\n", a);
            break;
        default:
            *error = std::format("unsupported opcode 0x{:02X}", op);
            return false;
        }
        return true;
    }

    // Structured emission of [pc, final) with the bool/int uniforms known: static branches
    // are resolved, CALLs inlined, LOOPs become counted for-loops (BREAK/BREAKC = break),
    // IFC/CALLC become ifs, JMPC an if/else over the rest of the frame (forward jumps only).
    // Returns false (with `error`) for flow it cannot structure.
    // `ended`: an END was reached on every path through this range.
    size_t budget = 32768;  // emitted instructions, bounds CALL inlining
    bool emit(u32 pc, u32 final, int loops, bool& ended, std::string* error) {
        const u32 n = u32(s.code.size());
        ended = false;
        while (pc != final) {
            if (pc >= n || pc > final || budget-- == 0) return *error = "unstructured or oversized control flow", false;
            const u32 w = s.code[pc], op = w >> 26;
            const u32 dst = bits(w, 10, 12), num = bits(w, 0, 8);
            auto buniform = [&] { return bool(s.bools >> bits(w, 22, 4) & 1); };
            auto nested = [&](u32 from, u32 to, int l) {  // returns ended, or nullopt on failure
                bool e = false;
                return emit(from, to, l, e, error) ? std::optional<bool>(e) : std::nullopt;
            };
            ++pc;
            switch (op) {
            case NOP: break;
            case END: body += "        return;\n"; ended = true; return true;
            case BREAK:
                if (!loops) return *error = "BREAK outside a loop", false;
                body += "        break;\n";
                return true;  // the rest of this range is dead
            case BREAKC:
                if (!loops) return *error = "BREAKC outside a loop", false;
                body += std::format("        if ({}) break;\n", cond(w));
                break;
            case CALL: case CALLU: case CALLC: {
                if (op == CALLU && !buniform()) break;
                if (op == CALLC) body += std::format("        if ({}) {{\n", cond(w));
                const auto e = nested(dst, dst + num, loops);
                if (!e) return false;
                if (op == CALLC) body += "        }\n";
                else if (*e) return ended = true, true;
                break;
            }
            case IFU: {
                const auto e = buniform() ? nested(pc, dst, loops) : nested(dst, dst + num, loops);
                if (!e) return false;
                if (*e) return ended = true, true;
                pc = dst + num;
                break;
            }
            case IFC: {
                body += std::format("        if ({}) {{\n", cond(w));
                const auto a = nested(pc, dst, loops);
                if (!a) return false;
                body += "        } else {\n";
                const auto b = nested(dst, dst + num, loops);
                if (!b) return false;
                body += "        }\n";
                if (*a && *b) return ended = true, true;
                pc = dst + num;
                break;
            }
            case LOOP: {
                const u32 iu = s.ints[bits(w, 22, 2)];
                body += std::format("        aL = {};\n        for (int k{} = 0; k{} <= {}; ++k{}) {{\n", iu >> 8 & 0xFF,
                                    pc, pc, iu & 0xFF, pc);
                if (!nested(pc, dst + 1, loops + 1)) return false;
                body += std::format("        aL += {};\n        }}\n", int(s8(iu >> 16)));
                pc = dst + 1;
                break;
            }
            case JMPU:
            case JMPC:
                if (op == JMPU && buniform() == bool(num & 1)) break;  // not taken
                if (dst < pc || dst > final)
                    return *error = std::format("backward or out-of-frame jump at {} to {} (frame ends at {})", pc - 1,
                                                dst, final),
                           false;
                if (op == JMPC) {  // both paths continue to the end of the frame (tail duplicated)
                    body += std::format("        if ({}) {{\n", cond(w));
                    const auto a = nested(dst, final, loops);
                    if (!a) return false;
                    body += "        } else {\n";
                    const auto b = nested(pc, final, loops);
                    if (!b) return false;
                    body += "        }\n";
                    ended = *a && *b;
                    return true;
                }
                pc = dst;
                break;
            case EMIT: case SETEMIT: return *error = "geometry shader opcodes (EMIT/SETEMIT) are not supported", false;
            case LITP: return *error = "LITP is not supported", false;
            default:
                if (!arith(w, error)) return false;
            }
        }
        return true;
    }

    std::string run(std::string* error) {
        bool ended = false;
        if (!emit(s.entry, ~0u, 0, ended, error)) return {};

        std::string out = "#version 450\n";
        for (u32 r : inputs_read) out += std::format("layout(location = {}) in vec4 vs_in{};\n", r, r);
        out += R"(
layout(std140, set = 0, binding = 0) uniform PicaVsUniforms { vec4 c[96]; ivec4 i[4]; vec4 depth; uint b; } u;

layout(location = 0) out vec4 primary_color;
layout(location = 1) out vec2 texcoord0;
layout(location = 2) out vec2 texcoord1;
layout(location = 3) out vec2 texcoord2;
layout(location = 4) out float texcoord0_w;
layout(location = 5) out vec4 normquat;
layout(location = 6) out vec3 view;

bvec4 isnan_or(bvec4 x, bvec4 y) { return bvec4(x.x || y.x, x.y || y.y, x.z || y.z, x.w || y.w); }
// PICA multiplication: 0 * inf = 0 (only NaN inputs give NaN).
vec4 sanitize_mul(vec4 a, vec4 b) {
    vec4 p = a * b;
    return mix(p, mix(vec4(0.0), p, isnan_or(isnan(a), isnan(b))), isnan(p));
}
float dot3_s(vec4 a, vec4 b) { vec4 p = sanitize_mul(a, b); return p.x + p.y + p.z; }
float dot4_s(vec4 a, vec4 b) { vec4 p = sanitize_mul(a, b); return p.x + p.y + p.z + p.w; }
vec4 dst_op(vec4 a, vec4 b) { return vec4(1.0, a.y * b.y, a.z, b.w); }
vec4 max_op(vec4 a, vec4 b) { return mix(b, a, greaterThan(a, b)); }
vec4 min_op(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }

vec4 v[16], r[16], o[16];
ivec2 a0 = ivec2(0);
int aL = 0;
bvec2 cc = bvec2(false);

void run() {
)";
        out += body;
        out += "}\n\nvoid main() {\n    for (int k = 0; k < 16; ++k) { v[k] = vec4(0.0); r[k] = vec4(0.0); o[k] = vec4(0.0); }\n";
        for (u32 r : inputs_read) out += std::format("    v[{}] = vs_in{};\n", r, r);
        out += "    run();\n";

        // Pack o0..o6 into the fixed varyings according to VS_OUTMAP.
        out += "    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0);\n    vec4 col = vec4(0.0);\n"
               "    vec4 quat = vec4(0.0);\n    vec2 t0 = vec2(0.0), t1 = vec2(0.0), t2 = vec2(0.0);\n"
               "    float t0w = 0.0;\n    vec3 vw = vec3(0.0);\n";
        static const char* sem[0x18] = {
            "pos.x", "pos.y", "pos.z", "pos.w", "quat.x", "quat.y", "quat.z", "quat.w",
            "col.r", "col.g", "col.b", "col.a", "t0.x", "t0.y", "t1.x", "t1.y",
            "t0w", nullptr, "vw.x", "vw.y", "vw.z", nullptr, "t2.x", "t2.y"};
        for (u32 reg = 0; reg < 7 && reg < s.outmap.size(); ++reg)
            for (u32 c = 0; c < 4; ++c) {
                const u32 id = s.outmap[reg] >> (c * 8) & 0x1F;
                if (id < 0x18 && sem[id]) out += std::format("    {} = o[{}].{};\n", sem[id], reg, "xyzw"[c]);
            }
        // PICA window depth = z/w * scale + offset (depth.x/.y from regs 0x4D/0x4E); Vulkan's
        // depth range is the identity, so pre-multiply by w.
        out += R"(    gl_Position = vec4(pos.x, pos.y, pos.z * u.depth.x + pos.w * u.depth.y, pos.w);
    primary_color = min(abs(col), vec4(1.0));
    texcoord0 = t0;
    texcoord1 = t1;
    texcoord2 = t2;
    texcoord0_w = t0w;
    normquat = quat;
    view = vw;
}
)";
        return out;
    }
};

}  // namespace

std::string generate_vertex_shader(const VsSource& src, std::string* error) {
    std::string err;
    Translator t{src, {}, {}};
    std::string out = t.run(&err);
    if (error) *error = err;
    return out;
}

// ---------------------------------------------------------------- fragment / TEV

namespace {

constexpr u32 TEV_STAGE_REGS[6] = {0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8};
constexpr u32 TEV_BUFFER_INPUT = 0xE0, TEV_BUFFER_COLOR = 0xFD, ALPHA_TEST = 0x104;

std::string tev_source(u32 id) {
    switch (id) {
    case 0x0: return "primary_color";
    case 0x1: return "primary_fragment_color";
    case 0x2: return "secondary_fragment_color";
    case 0x3: return "tex_color0";
    case 0x4: return "tex_color1";
    case 0x5: return "tex_color2";
    case 0x6: return "vec4(0.0)";  // shortcut: procedural texture unit
    case 0xD: return "combiner_buffer";
    case 0xE: return "const_color";
    default: return "last_tex_env_out";  // 0xF previous
    }
}

std::string color_operand(u32 op, const std::string& s) {
    switch (op) {
    case 0x0: return s + ".rgb";
    case 0x1: return "(vec3(1.0) - " + s + ".rgb)";
    case 0x2: return s + ".aaa";
    case 0x3: return "(vec3(1.0) - " + s + ".aaa)";
    case 0x4: return s + ".rrr";
    case 0x5: return "(vec3(1.0) - " + s + ".rrr)";
    case 0x8: return s + ".ggg";
    case 0x9: return "(vec3(1.0) - " + s + ".ggg)";
    case 0xC: return s + ".bbb";
    case 0xD: return "(vec3(1.0) - " + s + ".bbb)";
    default: return s + ".rgb";
    }
}

std::string alpha_operand(u32 op, const std::string& s) {
    static const char* comp[] = {"a", "r", "g", "b"};
    const std::string v = s + "." + comp[(op >> 1) & 3];
    return op & 1 ? "(1.0 - " + v + ")" : v;
}

// `T` is "vec3" or "float"; a/b/c are operand expressions.
std::string combine(u32 op, const std::string& T, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
    case 0: return a;
    case 1: return a + " * " + b;
    case 2: return a + " + " + b;
    case 3: return a + " + " + b + " - " + T + "(0.5)";
    case 4: return a + " * " + c + " + " + b + " * (" + T + "(1.0) - " + c + ")";
    case 5: return a + " - " + b;
    case 6: case 7: return T + "(4.0 * dot(" + a + " - vec3(0.5), " + b + " - vec3(0.5)))";
    case 8: return a + " * " + b + " + " + c;
    case 9: return "min(" + a + " + " + b + ", " + T + "(1.0)) * " + c;
    default: return a;
    }
}

// ---------------------------------------------------------------- fragment lighting
//
// Structure (enabled lights, LUT inputs/scales, configuration) is baked into the shader;
// light colours, positions and the LUTs are read at runtime from the PicaLighting buffer,
// which holds raw registers 0x140..0x1C0 and ShaderState::light_lut. Follows the hardware
// model documented by Citra (PICA200 lighting).

constexpr u32 LIGHTING_ENABLE = 0x08F, LIGHT0 = 0x140, LIGHT_MAX_INDEX = 0x1C2, LIGHT_CONFIG0 = 0x1C3,
              LIGHT_CONFIG1 = 0x1C4, LIGHTING_DISABLE = 0x1C6, LUT_ABS = 0x1D0, LUT_SELECT = 0x1D1,
              LUT_SCALE = 0x1D2, LIGHT_PERMUTATION = 0x1D9;

constexpr const char* LIGHTING_DECLS = R"(layout(std430, set = 0, binding = 1) readonly buffer PicaLighting { uint lr[0x90]; uint lut[24 * 256]; };
vec3 light_color(uint r) { return vec3((r >> 20) & 0x3FFu, (r >> 10) & 0x3FFu, r & 0x3FFu) / 255.0; }
float f16(uint v) { return unpackHalf2x16(v & 0xFFFFu).x; }
float f20(uint v) {  // 1.7.12 float
    if ((v & 0x7FFFFu) == 0u) return 0.0;
    float m = ldexp(1.0 + float(v & 0xFFFu) / 4096.0, int((v >> 12) & 0x7Fu) - 63);
    return (v & 0x80000u) != 0u ? -m : m;
}
float lut_entry(int t, int i, float delta) {
    uint e = lut[t * 256 + i];
    return float(e & 0xFFFu) / 4095.0 + float(bitfieldExtract(int(e), 12, 12)) / 2047.0 * delta;
}
float lut_unsigned(int t, float pos) {
    int i = int(clamp(floor(pos * 256.0), 0.0, 255.0));
    return lut_entry(t, i, pos * 256.0 - float(i));
}
float lut_signed(int t, float pos) {
    int i = int(clamp(floor(pos * 128.0), -128.0, 127.0));
    return lut_entry(t, i & 255, pos * 128.0 - float(i));
}
vec3 quaternion_rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
)";

bool lighting_enabled(const std::array<u32, reg::COUNT>& regs) {
    return (regs[LIGHTING_ENABLE] & 1) && !(regs[LIGHTING_DISABLE] & 1);
}

// Computes primary/secondary_fragment_color (declared by the caller).
// shortcut: no shadow maps (shadow bits ignored); add when a scene renders shadows.
std::string lighting(const std::array<u32, reg::COUNT>& regs) {
    const u32 c0 = regs[LIGHT_CONFIG0], c1 = regs[LIGHT_CONFIG1], cfg = c0 >> 4 & 0xF;
    // LUT ids and their nibble in LUT_ABS/SELECT/SCALE.
    enum { D0 = 0, D1 = 1, FR = 3, RB = 4, RG = 5, RR = 6, SP = 8 };
    auto shift = [](int t) { return t == D0 ? 0 : t == D1 ? 4 : t == SP ? 8 : t == FR ? 12 : t == RB ? 16 : t == RG ? 20 : 24; };
    auto supported = [&](int t) {  // tables each LIGHT_CONFIG0 configuration provides (Config7 = 8)
        switch (t) {
        case D0: return cfg != 1;
        case D1: return cfg != 0 && cfg != 1 && cfg != 5;
        case FR: return cfg != 0 && cfg != 2 && cfg != 4;
        case RR: return cfg != 3;
        case RG: case RB: return cfg == 4 || cfg == 5 || cfg == 8;
        default: return cfg != 2 && cfg != 3;  // SP
        }
    };
    auto enabled = [&](int t) {  // global LUT disable bits in LIGHT_CONFIG1 (SP is per light)
        const int bit = t == D0 ? 16 : t == D1 ? 17 : t == FR ? 19 : t == RR ? 20 : t == RG ? 21 : 22;
        return supported(t) && !(c1 >> bit & 1);
    };
    // Scaled LUT lookup of table `t` (sampler `sampler`) for the light whose config is `lcfg`.
    auto lookup = [&](int t, int sampler, u32 lcfg) {
        static const char* INPUT[] = {"dot(normal, normalize(half_vector))", "dot(normalize(view), normalize(half_vector))",
                                      "dot(normal, normalize(view))", "dot(light_vector, normal)",
                                      "dot(light_vector, spot_dir)"};
        static const float SCALE[8] = {1, 2, 4, 8, 1, 1, 0.25f, 0.5f};
        const u32 sh = shift(t), in = regs[LUT_SELECT] >> sh & 7;
        std::string index = in < 5 ? INPUT[in]
                            : in == 5 && cfg == 8
                                ? "dot(normalize(half_vector) - normal * dot(normal, normalize(half_vector)), tangent)"
                                : "0.0";
        const bool abs = !(regs[LUT_ABS] >> (sh + 1) & 1);
        const std::string value = abs ? std::format("lut_unsigned({}, {}({}{}))", sampler, lcfg >> 1 & 1 ? "abs" : "max",
                                                    index, lcfg >> 1 & 1 ? "" : ", 0.0")
                                      : std::format("lut_signed({}, {})", sampler, index);
        return std::format("({:.4f} * {})", SCALE[regs[LUT_SCALE] >> sh & 7], value);
    };

    std::string out = R"(    vec3 surface_normal = vec3(0.0, 0.0, 1.0), surface_tangent = vec3(1.0, 0.0, 0.0);
)";
    const u32 bump_mode = c0 >> 28 & 3, bump_tex = std::min<u32>(c0 >> 22 & 3, 2);
    if (bump_mode == 1 || bump_mode == 2) {  // normal map / tangent map from a texture unit
        const char* v = bump_mode == 1 ? "surface_normal" : "surface_tangent";
        out += std::format("    {} = 2.0 * tex_color{}.rgb - 1.0;\n", v, bump_tex);
        if (!(c0 >> 30 & 1)) out += std::format("    {0}.z = sqrt(max(1.0 - dot({0}.xy, {0}.xy), 0.0));\n", v);
    }
    out += R"(    vec4 nq = normalize(normquat);
    vec3 normal = quaternion_rotate(nq, surface_normal);
    vec3 tangent = quaternion_rotate(nq, surface_tangent);
    vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0), specular_sum = vec4(0.0, 0.0, 0.0, 1.0);
    vec3 light_vector, spot_dir, half_vector, refl_value;
)";
    const u32 count = (regs[LIGHT_MAX_INDEX] & 7) + 1;
    for (u32 i = 0; i < count; ++i) {
        const u32 n = regs[LIGHT_PERMUTATION] >> (4 * i) & 7, b = n * 16, lcfg = regs[LIGHT0 + b + 9];
        out += std::format(R"(    {{  // light {0}
    vec3 lpos = vec3(f16(lr[{1}]), f16(lr[{1}] >> 16), f16(lr[{2}]));
    light_vector = normalize({3});
    spot_dir = vec3(bitfieldExtract(int(lr[{4}]), 0, 13), bitfieldExtract(int(lr[{4}]), 16, 13),
                    bitfieldExtract(int(lr[{5}]), 0, 13)) / 2047.0;
    half_vector = normalize(view) + light_vector;
    float dot_product = {6};
    float geo_factor = dot(half_vector, half_vector);
    geo_factor = geo_factor == 0.0 ? 0.0 : min(dot_product / geo_factor, 1.0);
)",
                           n, b + 4, b + 5, lcfg & 1 ? "lpos" : "lpos + view", b + 6, b + 7,
                           lcfg >> 1 & 1 ? "abs(dot(light_vector, normal))" : "max(dot(light_vector, normal), 0.0)");
        const bool spot = !(c1 >> (8 + n) & 1) && supported(SP), dist = !(c1 >> (24 + n) & 1);
        out += std::format("    float spot_atten = {};\n", spot ? lookup(SP, SP + n, lcfg) : "1.0");
        out += dist ? std::format("    float dist_atten = lut_unsigned({}, clamp(f20(lr[{}]) * length(-view - lpos) + "
                                  "f20(lr[{}]), 0.0, 1.0));\n",
                                  16 + n, b + 11, b + 10)
                    : "    float dist_atten = 1.0;\n";
        out += std::format("    vec3 specular_0 = {} * light_color(lr[{}]){};\n", enabled(D0) ? lookup(D0, D0, lcfg) : "1.0",
                           b, lcfg >> 2 & 1 ? " * geo_factor" : "");
        out += std::format("    refl_value.r = {};\n", enabled(RR) ? lookup(RR, RR, lcfg) : "1.0");
        out += std::format("    refl_value.g = {};\n", enabled(RG) ? lookup(RG, RG, lcfg) : "refl_value.r");
        out += std::format("    refl_value.b = {};\n", enabled(RB) ? lookup(RB, RB, lcfg) : "refl_value.r");
        out += std::format("    vec3 specular_1 = {} * refl_value * light_color(lr[{}]){};\n",
                           enabled(D1) ? lookup(D1, D1, lcfg) : "1.0", b + 1, lcfg >> 3 & 1 ? " * geo_factor" : "");
        if (enabled(FR)) {  // Fresnel replaces alpha; the last light's value wins
            const std::string fr = lookup(FR, FR, lcfg);
            if (c0 >> 2 & 1) out += std::format("    diffuse_sum.a = {};\n", fr);
            if (c0 >> 3 & 1) out += std::format("    specular_sum.a = {};\n", fr);
        }
        out += std::format(R"(    float clamp_highlights = {};
    diffuse_sum.rgb += (light_color(lr[{}]) * dot_product + light_color(lr[{}])) * dist_atten * spot_atten;
    specular_sum.rgb += (specular_0 + specular_1) * clamp_highlights * dist_atten * spot_atten;
    }}
)",
                           c0 >> 27 & 1 ? "dot(light_vector, normal) <= 0.0 ? 0.0 : 1.0" : "1.0", b + 2, b + 3);
    }
    out += R"(    diffuse_sum.rgb += light_color(lr[0x80]);
    primary_fragment_color = clamp(diffuse_sum, 0.0, 1.0);
    secondary_fragment_color = clamp(specular_sum, 0.0, 1.0);
)";
    return out;
}

}  // namespace

std::array<u32, FRAGMENT_PUSH_WORDS> fragment_push_constants(const std::array<u32, reg::COUNT>& regs) {
    std::array<u32, FRAGMENT_PUSH_WORDS> pc{};
    for (u32 i = 0; i < 6; ++i) pc[i] = regs[TEV_STAGE_REGS[i] + 3];
    pc[6] = regs[TEV_BUFFER_COLOR];
    pc[7] = regs[ALPHA_TEST] >> 8 & 0xFF;
    return pc;
}

std::string generate_fragment_shader(const std::array<u32, reg::COUNT>& regs) {
    std::string out = R"(#version 450
layout(location = 0) in vec4 primary_color;
layout(location = 1) in vec2 texcoord0;
layout(location = 2) in vec2 texcoord1;
layout(location = 3) in vec2 texcoord2;
layout(location = 4) in float texcoord0_w;
layout(location = 5) in vec4 normquat;
layout(location = 6) in vec3 view;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(set = 1, binding = 2) uniform sampler2D tex2;
layout(location = 0) out vec4 frag_color;
layout(push_constant) uniform PicaFragment { uint tev_const[6]; uint tev_buffer_color; uint alpha_ref; } pc;
)";
    const bool lit = lighting_enabled(regs);
    if (lit) out += LIGHTING_DECLS;
    out += R"(
void main() {
    vec4 tex_color0 = texture(tex0, texcoord0);
    vec4 tex_color1 = texture(tex1, texcoord1);
)";
    // TEXUNIT_CONFIG bit 13: texture 2 samples texcoord1 instead of texcoord2.
    out += std::format("    vec4 tex_color2 = texture(tex2, {});\n", regs[0x80] >> 13 & 1 ? "texcoord1" : "texcoord2");
    out += R"(    vec4 primary_fragment_color = vec4(0.0);
    vec4 secondary_fragment_color = vec4(0.0);
    vec4 combiner_buffer = vec4(0.0);
    vec4 next_combiner_buffer = unpackUnorm4x8(pc.tev_buffer_color);
    vec4 last_tex_env_out = vec4(0.0);
    vec4 const_color;
)";
    if (lit) out += lighting(regs);

    static const float SCALE[4] = {1.0f, 2.0f, 4.0f, 1.0f};
    for (u32 i = 0; i < 6; ++i) {
        const u32 base = TEV_STAGE_REGS[i];
        const u32 srcs = regs[base], ops = regs[base + 1], comb = regs[base + 2], scale = regs[base + 4];
        const u32 color_op = comb & 0xF, alpha_op = comb >> 16 & 0xF;
        out += std::format("    // TEV stage {}\n    {{\n", i);
        out += std::format("    const_color = unpackUnorm4x8(pc.tev_const[{}]);\n", i);
        std::string ca[3], aa[3];
        for (u32 k = 0; k < 3; ++k) {
            ca[k] = color_operand(ops >> (4 * k) & 0xF, tev_source(srcs >> (4 * k) & 0xF));
            aa[k] = alpha_operand(ops >> (12 + 4 * k) & 0x7, tev_source(srcs >> (16 + 4 * k) & 0xF));
        }
        out += std::format("    vec3 color_result = clamp(({}) * {:.1f}, 0.0, 1.0);\n",
                           combine(color_op, "vec3", ca[0], ca[1], ca[2]), SCALE[scale & 3]);
        if (color_op == 7)  // DOT3_RGBA writes alpha too
            out += "    float alpha_result = color_result.r;\n";
        else
            out += std::format("    float alpha_result = clamp(({}) * {:.1f}, 0.0, 1.0);\n",
                               combine(alpha_op, "float", aa[0], aa[1], aa[2]), SCALE[scale >> 16 & 3]);
        out += "    last_tex_env_out = vec4(color_result, alpha_result);\n";
        out += "    combiner_buffer = next_combiner_buffer;\n";
        if (i < 4) {
            if (regs[TEV_BUFFER_INPUT] >> (8 + i) & 1) out += "    next_combiner_buffer.rgb = last_tex_env_out.rgb;\n";
            if (regs[TEV_BUFFER_INPUT] >> (12 + i) & 1) out += "    next_combiner_buffer.a = last_tex_env_out.a;\n";
        }
        out += "    }\n";
    }

    const u32 at = regs[ALPHA_TEST];
    if (at & 1) {
        static const char* keep[] = {"false", "true", "==", "!=", "<", "<=", ">", ">="};
        const u32 func = at >> 4 & 7;
        if (func == 0) out += "    discard;\n";
        else if (func > 1)
            out += std::format("    if (!(int(last_tex_env_out.a * 255.0) {} int(pc.alpha_ref))) discard;\n", keep[func]);
    }
    out += "    frag_color = last_tex_env_out;\n}\n";
    return out;
}

}  // namespace gfx::pica
