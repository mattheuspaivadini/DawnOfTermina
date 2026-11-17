# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(DawnOfTermina LANGUAGES CXX)

# An unoptimised runtime (VS interpreter, renderer, kernel) runs the game several times slower.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL3 REQUIRED CONFIG)
find_package(Vulkan REQUIRED)
find_package(glslang REQUIRED CONFIG)

# Dear ImGui (MIT) for the in-game settings menu, pinned by hash. Offline builds can point
# FETCHCONTENT_SOURCE_DIR_IMGUI at a local copy of the same release.
include(FetchContent)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz
    URL_HASH SHA256=3872a5f90df78fced023c1945f4466b654fd74573370b77b17742149763a7a7c)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)

add_library(dot_runtime STATIC
    src/core/memory.cpp
    src/core/loader.cpp
    src/core/config.cpp
    src/hle/svc.cpp
    src/hle/srv.cpp
    src/hle/hid.cpp
    src/hle/dsp.cpp
    src/gfx/pica/texture.cpp
    src/gfx/pica/command_processor.cpp
    src/gfx/pica/shbin.cpp
    src/gfx/pica/shader_gen.cpp
    src/gfx/vulkan/spirv.cpp
    src/gfx/vulkan/renderer.cpp
    src/gfx/pica/vertex_loader.cpp
    src/gfx/pica/shader_interp.cpp
    src/gfx/vulkan/context.cpp
    src/hle/kernel.cpp
    src/hle/service/services.cpp
    src/hle/service/apt.cpp
    src/hle/service/misc.cpp
    src/hle/service/ir.cpp
    src/hle/service/fs.cpp
    src/hle/service/cfg.cpp
    src/hle/service/gsp.cpp
    src/hle/service/hid.cpp
    src/hle/service/dsp.cpp
    src/recomp/hooks.cpp
    src/recomp/dekui.cpp
    src/recomp/dekui_settings.cpp
    ${CMAKE_BINARY_DIR}/dekui_presentation.cpp
    src/recomp/runtime.cpp
    src/recomp/svc.cpp
)
# DekUI's default presentation.json, embedded (src/recomp/dekui_settings.cpp).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS resources/dekui/presentation.json)
file(READ resources/dekui/presentation.json DOT_DEKUI_PRESENTATION)
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/dekui_presentation.cpp @ONLY CONTENT
    "extern const char* const kDekuiPresentation;\nconst char* const kDekuiPresentation = R\"json(@DOT_DEKUI_PRESENTATION@)json\";\n")
target_include_directories(dot_runtime PUBLIC src)
# The renderer's fixed shaders (src/gfx/vulkan/shaders/) are compiled to SPIR-V here, not at run time:
# embedded as C arrays (shaders/<name>.h) and also installed as .spv into dist/shaders/.
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(DOT_SHADER_HEADERS)
set(DOT_SHADER_SPV)
foreach(src pass.vert transfer.comp)
    string(REPLACE "." "_" name ${src})
    set(in ${CMAKE_SOURCE_DIR}/src/gfx/vulkan/shaders/${src})
    set(h ${CMAKE_BINARY_DIR}/shaders/${name}.h)
    set(spv ${CMAKE_BINARY_DIR}/shaders/${src}.spv)
    add_custom_command(OUTPUT ${h} ${spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --quiet --vn k_${name} -o ${h} ${in}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --quiet -o ${spv} ${in}
        DEPENDS ${in} VERBATIM)
    list(APPEND DOT_SHADER_HEADERS ${h})
    list(APPEND DOT_SHADER_SPV ${spv})
endforeach()
target_sources(dot_runtime PRIVATE ${DOT_SHADER_HEADERS})
target_include_directories(dot_runtime PRIVATE ${CMAKE_BINARY_DIR})
target_include_directories(dot_runtime SYSTEM PRIVATE src/dekui)  # DekUI's headers, verbatim (GPL-2.0-or-later)
target_link_libraries(dot_runtime PUBLIC SDL3::SDL3 Vulkan::Vulkan
    glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
target_compile_options(dot_runtime PUBLIC -Wall -Wextra -Wno-missing-field-initializers)

add_executable(dawnoftermina src/main.cpp src/ui/menu.cpp)
target_link_libraries(dawnoftermina PRIVATE dot_runtime imgui)

# Static recompilation of the user's code.bin (tools/recomp). Generated at build time
# into the build tree only: it is derived from the game and must never be committed.
# Majora's Mask 3D v1.1 only: game data extracted from the base .cia + the v1.1 update .cia
# (tools/extract_cia.py <game.cia> <dir> --update <update_v1.1.cia>), mods over v1.1 allowed.
set(DOT_GAME_DATA "${CMAKE_SOURCE_DIR}/game_data" CACHE PATH "Extracted v1.1 game data (tools/extract_cia.py)")
set(DOT_RECOMP_CHUNKS 64)
set(manifest_file "${DOT_GAME_DATA}/manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest_file}")
if(EXISTS "${manifest_file}" AND EXISTS "${DOT_GAME_DATA}/exefs/code.bin")
    file(READ "${manifest_file}" manifest)
    string(JSON game_version ERROR_VARIABLE json_error GET "${manifest}" game_version)
    string(JSON code_sha256 ERROR_VARIABLE json_error GET "${manifest}" code_sha256)
    # Built with Project Restoration (+ HD HUD/DekUI): the launcher downloads and applies the same mods.
    string(JSON dot_modded ERROR_VARIABLE json_error GET "${manifest}" restoration_enabled)
endif()
if(NOT game_version STREQUAL "1.1" OR NOT code_sha256)
    message(FATAL_ERROR "DOT_GAME_DATA (${DOT_GAME_DATA}) is not a Majora's Mask 3D v1.1 extraction "
        "(manifest.json with game_version 1.1). Extract the base game and the v1.1 update:\n"
        "  python3 tools/extract_cia.py <game.cia> <dir> --update <update_v1.1.cia>\n"
        "and configure with -DDOT_GAME_DATA=<dir>.")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(gen "${CMAKE_BINARY_DIR}/recomp_gen")
set(gen_sources "${gen}/recomp_table.cpp")
math(EXPR last "${DOT_RECOMP_CHUNKS} - 1")
foreach(i RANGE ${last})
    if(i LESS 10)
        list(APPEND gen_sources "${gen}/recomp_0${i}.cpp")
    else()
        list(APPEND gen_sources "${gen}/recomp_${i}.cpp")
    endif()
endforeach()
file(GLOB recomp_tool CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/recomp/*.py")
add_custom_command(
    OUTPUT ${gen_sources} "${gen}/recomp_funcs.h"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/recomp/recomp.py" "${DOT_GAME_DATA}" "${gen}"
            --chunks ${DOT_RECOMP_CHUNKS}
    DEPENDS ${recomp_tool} "${DOT_GAME_DATA}/exefs/code.bin" "${DOT_GAME_DATA}/exheader.bin"
            "${CMAKE_SOURCE_DIR}/tools/recomp/extra_entries.txt" "${CMAKE_SOURCE_DIR}/tools/recomp/hooks.txt"
    COMMENT "Recompiling code.bin (ARM11 -> C++)"
    VERBATIM)
add_library(dot_game STATIC ${gen_sources})
target_include_directories(dot_game PRIVATE src "${gen}")
target_link_libraries(dot_game PRIVATE dot_runtime)  # native hooks (src/recomp/hooks.cpp)
# Generated code: VFP MAC must not be fused; guest memory is accessed through u8*.
target_compile_options(dot_game PRIVATE -O1 -w -ffp-contract=off -fno-strict-aliasing)
target_link_libraries(dawnoftermina PRIVATE dot_game)
# The executable runs only the code.bin it was compiled from (checked against manifest.json at start).
target_compile_definitions(dawnoftermina PRIVATE DOT_CODE_SHA256="${code_sha256}")
if(dot_modded)
    target_compile_definitions(dawnoftermina PRIVATE DOT_DOWNLOAD_MODS=1)
endif()

# Self-contained folder: cmake --install build --prefix dist. With game data configured the
# executable holds the recompiled game code: personal use only, never commit or share it.
install(TARGETS dawnoftermina RUNTIME DESTINATION .)
install(FILES tools/extract_cia.py tools/bps_patch.py tools/romfs_patch.py DESTINATION tools)
install(FILES LICENSE README.md DESTINATION .)
install(FILES ${DOT_SHADER_SPV} DESTINATION shaders)
# Pipelines to compile right after extraction (generator inputs only; tools/merge_seed.py).
if(EXISTS ${CMAKE_SOURCE_DIR}/resources/pipeline_seed.bin)
    install(FILES resources/pipeline_seed.bin DESTINATION .)
endif()

enable_testing()
add_executable(dot_selftest tests/selftest.cpp tests/pica_test.cpp tests/shader_test.cpp
    tests/recomp_test.cpp tests/audio_test.cpp)
target_link_libraries(dot_selftest PRIVATE dot_runtime)
target_include_directories(dot_selftest SYSTEM PRIVATE src/dekui)
add_test(NAME selftest COMMAND dot_selftest ${DOT_GAME_DATA})
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME recomp_lifter COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recomp/test_lifter.py)
    add_test(NAME tool_versions COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/test_versions.py)
    add_test(NAME tool_bps COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/test_bps.py)
    add_test(NAME tool_romfs COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/test_romfs.py)
endif()
