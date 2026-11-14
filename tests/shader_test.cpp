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

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "gfx/pica/shader_gen.h"
#include "gfx/pica/shbin.h"
#include "gfx/vulkan/spirv.h"

using namespace gfx::pica;
using gfx::vulkan::compile_glsl;
using gfx::vulkan::ShaderStage;
namespace stdfs = std::filesystem;

static bool compiles(const std::string& glsl, ShaderStage stage) {
    std::string log;
    const auto spv = compile_glsl(glsl, stage, &log);
    if (spv.empty()) std::fprintf(stderr, "---- GLSL ----\n%s\n---- glslang ----\n%s\n", glsl.c_str(), log.c_str());
    return !spv.empty() && spv[0] == 0x07230203;  // SPIR-V magic
}

static constexpr u32 IDENT = 0x1B;  // swizzle .xyzw
static constexpr u32 DESC_XYZW = 0xF | IDENT << 5 | IDENT << 14 | IDENT << 23;
static const std::vector<u32> OUTMAP = {0x03020100, 0x0B0A0908, 0x1F1F0D0C, 0x1F1F1F1F,
                                        0x1F1F1F1F, 0x1F1F1F1F, 0x1F1F1F1F};

static void test_synthetic_vs() {
    const std::vector<u32> desc = {DESC_XYZW};
    // MOV o0, v0 ; MOV o1, c0 ; END
    const std::vector<u32> simple = {0x13u << 26 | 0 << 21 | 0 << 12, 0x13u << 26 | 1 << 21 | 0x20 << 12, 0x22u << 26};
    std::string err;
    const std::string glsl = generate_vertex_shader({simple, desc, 0, OUTMAP}, &err);
    assert(err.empty() && glsl.find("o[0].xyzw = (v[0].xyzw).xyzw;") != std::string::npos);
    assert(glsl.find("layout(location = 0) in vec4 vs_in0;") != std::string::npos);
    assert(compiles(glsl, ShaderStage::Vertex));

    // Control flow: LOOP (relative uniform via aL), IFU, CALL, MUL, END.
    const std::vector<u32> flow = {
        0x29u << 26 | 0 << 22 | 2 << 10,                                // 0: LOOP i0, body 1..2
        0x00u << 26 | 0x10 << 21 | 3 << 19 | 0x20 << 12 | 0x10 << 7,    // 1: ADD r0, c[0+aL], r0
        0x21u << 26,                                                    // 2: NOP
        0x27u << 26 | 0 << 22 | 5 << 10 | 1,                            // 3: IFU b0, else 5..5
        0x24u << 26 | 7 << 10 | 1,                                      // 4: CALL 7..7
        0x13u << 26 | 0 << 21 | 0x10 << 12,                             // 5: MOV o0, r0
        0x22u << 26,                                                    // 6: END
        0x08u << 26 | 0x11 << 21 | 0x10 << 12 | 0x10 << 7,              // 7: MUL r1, r0, r0
        0x22u << 26,                                                    // 8: END
    };
    const std::string flow_glsl = generate_vertex_shader({flow, desc, 0, OUTMAP}, &err);
    assert(err.empty() && flow_glsl.find("u.c[clamp(0 + aL, 0, 95)]") != std::string::npos);
    assert(compiles(flow_glsl, ShaderStage::Vertex));

    // Specialised: b0 set, i0 = 3 iterations (x = 2), aL from 5 step 1.
    VsSource spec{flow, desc, 0, OUTMAP, 1, {2 | 5 << 8 | 1 << 16, 0, 0, 0}};
    const std::string spec_glsl = generate_vertex_shader(spec, &err);
    assert(spec_glsl.find("aL = 5;") != std::string::npos && spec_glsl.find("<= 2;") != std::string::npos);
    assert(spec_glsl.find("switch") == std::string::npos && compiles(spec_glsl, ShaderStage::Vertex));

    // Flow that cannot be structured is refused (the renderer then runs the CPU interpreter).
    const std::vector<u32> back = {0x21u << 26, 0x2Du << 26 | 0 << 10 | 1, 0x22u << 26};  // NOP ; JMPU (b0 != 1) -> 0
    assert(generate_vertex_shader({back, desc, 0, OUTMAP}, &err).empty() && err.find("jump") != std::string::npos);
    const std::vector<u32> brk = {0x20u << 26, 0x22u << 26};  // BREAK outside a loop
    assert(generate_vertex_shader({brk, desc, 0, OUTMAP}, &err).empty());

    const std::vector<u32> gs = {0x2Au << 26, 0x22u << 26};  // EMIT ; END
    assert(generate_vertex_shader({gs, desc, 0, OUTMAP}, &err).empty() && err.find("geometry") != std::string::npos);
}

static void test_fragment() {
    std::array<u32, reg::COUNT> regs{};
    assert(compiles(generate_fragment_shader(regs), ShaderStage::Fragment));

    // Stage 0: lerp(texture0, primary, const.a), stage 1: dot3_rgba, buffer updates, alpha test >= 128.
    regs[0xC0] = 0x3 | 0x0 << 4 | 0xE << 8 | 0x3 << 16;
    regs[0xC1] = 0x0 | 0x1 << 4 | 0x2 << 8;
    regs[0xC2] = 4 | 0 << 16;
    regs[0xC3] = 0x80FF8040;
    regs[0xC4] = 1 | 2 << 16;
    regs[0xC8] = 0xF | 0xD << 4;
    regs[0xCA] = 7;
    regs[0xE0] = 0x1 << 8 | 0x1 << 12;
    regs[0x104] = 1 | 7 << 4 | 128 << 8;
    const std::string fs = generate_fragment_shader(regs);
    assert(fs.find(">= 128") != std::string::npos);
    assert(fs.find("next_combiner_buffer.rgb = last_tex_env_out.rgb;") != std::string::npos);
    assert(compiles(fs, ShaderStage::Fragment));

    // Every colour/alpha combiner op compiles.
    for (u32 op = 0; op < 10; ++op) {
        regs[0xC2] = op | op << 16;
        assert(compiles(generate_fragment_shader(regs), ShaderStage::Fragment));
    }

    // Fragment lighting: every LUT configuration, 3 lights (spot, distance attenuation,
    // geometric factors, two-sided), normal map from texture 1, Fresnel into both alphas.
    regs[0x8F] = 1;
    regs[0x1C2] = 2;
    regs[0x1D9] = 0x210;
    regs[0x140 + 0x10 + 9] = 0xE;  // light 1: two-sided + both geometric factors
    regs[0x1D1] = 0x05432105;      // CP input for D0, every other input once
    regs[0x1D0] = 0x2;             // D0 signed, others abs
    for (u32 cfg : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u}) {
        regs[0x1C3] = cfg << 4 | 3 << 2 | 1 << 22 | 1u << 28 | 1 << 27;
        const std::string lit = generate_fragment_shader(regs);
        assert(lit.find("primary_fragment_color = clamp(diffuse_sum") != std::string::npos);
        assert(compiles(lit, ShaderStage::Fragment));
    }
    regs[0x1C6] = 1;  // disabled again
    assert(generate_fragment_shader(regs).find("diffuse_sum") == std::string::npos);
}

// The game's own shader binaries: every vertex program must translate and compile.
static void test_game_shaders(const stdfs::path& data) {
    const stdfs::path dir = data / "romfs" / "shaders";
    if (!stdfs::exists(dir)) {
        std::puts("selftest: game shaders not present, skipped");
        return;
    }
    u32 vs = 0, gs = 0;
    for (const auto& entry : stdfs::directory_iterator(dir)) {
        std::ifstream f(entry.path(), std::ios::binary);
        const std::vector<u8> bytes{std::istreambuf_iterator<char>(f), {}};
        auto shbin = parse_shbin(bytes);
        assert(shbin && !shbin->programs.empty());
        for (const auto& prog : shbin->programs) {
            std::string err;
            const auto outmap = outmap_from_outputs(prog);
            if (prog.type == ShaderProgram::GEOMETRY) {
                assert(generate_vertex_shader({shbin->code, shbin->swizzles, prog.main, outmap}, &err).empty());
                ++gs;
                continue;
            }
            for (u16 bools : {u16(0), u16(0xFFFF)}) {  // every bool-uniform branch both ways
                const std::string glsl =
                    generate_vertex_shader({shbin->code, shbin->swizzles, prog.main, outmap, bools}, &err);
                if (glsl.empty()) std::fprintf(stderr, "%s: %s\n", entry.path().filename().c_str(), err.c_str());
                assert(!glsl.empty() && compiles(glsl, ShaderStage::Vertex));
            }
            ++vs;
        }
    }
    assert(vs == 6 && gs == 1);
    std::printf("selftest: %u game vertex programs -> SPIR-V OK (%u geometry program skipped)\n", vs, gs);
}

void test_shaders(const char* game_data) {
    assert(!parse_shbin(std::vector<u8>{'D', 'V', 'L', 'B', 9, 0, 0, 0}));  // truncated
    test_synthetic_vs();
    test_fragment();
    test_game_shaders(game_data);
}
