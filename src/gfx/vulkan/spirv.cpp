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

#include "gfx/vulkan/spirv.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <mutex>

namespace gfx::vulkan {

std::vector<u32> compile_glsl(std::string_view source, ShaderStage stage, std::string* log) {
    static std::once_flag init;
    std::call_once(init, [] { glslang::InitializeProcess(); });

    const EShLanguage lang = stage == ShaderStage::Vertex     ? EShLangVertex
                             : stage == ShaderStage::Fragment ? EShLangFragment
                                                              : EShLangCompute;
    glslang::TShader shader(lang);
    const char* text = source.data();
    const int len = int(source.size());
    shader.setStringsWithLengths(&text, &len, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, msgs)) {
        if (log) *log = shader.getInfoLog();
        return {};
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        if (log) *log = program.getInfoLog();
        return {};
    }
    std::vector<u32> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv);
    return spirv;
}

}  // namespace gfx::vulkan
