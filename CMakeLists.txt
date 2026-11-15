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
    src/hle/fs.cpp
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
    src/game/guest.cpp
    src/game/crt0.cpp
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
    src/recomp/runtime.cpp
    src/recomp/svc.cpp
)
target_include_directories(dot_runtime PUBLIC src)
target_link_libraries(dot_runtime PUBLIC SDL3::SDL3 Vulkan::Vulkan
    glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
target_compile_options(dot_runtime PUBLIC -Wall -Wextra -Wno-missing-field-initializers)

add_executable(dawnoftermina src/main.cpp src/ui/menu.cpp)
target_link_libraries(dawnoftermina PRIVATE dot_runtime imgui)

# Static recompilation of the user's code.bin (tools/recomp). Generated at build time
# into the build tree only: it is derived from the game and must never be committed.
set(DOT_GAME_DATA "${CMAKE_SOURCE_DIR}/game_data" CACHE PATH "Extracted game data (tools/extract_cia.py)")
set(DOT_RECOMP_CHUNKS 64)
if(EXISTS "${DOT_GAME_DATA}/exefs/code.bin")
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
    target_compile_definitions(dawnoftermina PRIVATE DOT_HAVE_RECOMP=1)
endif()

# Self-contained folder: cmake --install build --prefix dist. With game data configured the
# executable holds the recompiled game code: personal use only, never commit or share it.
install(TARGETS dawnoftermina RUNTIME DESTINATION .)
install(FILES tools/extract_cia.py tools/bps_patch.py tools/romfs_patch.py DESTINATION tools)
install(FILES LICENSE README.md DESTINATION .)

enable_testing()
add_executable(dot_selftest tests/selftest.cpp tests/pica_test.cpp tests/shader_test.cpp
    tests/recomp_test.cpp tests/audio_test.cpp)
target_link_libraries(dot_selftest PRIVATE dot_runtime)
add_test(NAME selftest COMMAND dot_selftest ${CMAKE_SOURCE_DIR}/game_data)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME recomp_lifter COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recomp/test_lifter.py)
    add_test(NAME tool_versions COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/test_versions.py)
    add_test(NAME tool_bps COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/test_bps.py)
    add_test(NAME tool_romfs COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/test_romfs.py)
endif()
