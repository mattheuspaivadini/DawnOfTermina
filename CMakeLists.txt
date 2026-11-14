# DawnOfTermina — SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(DawnOfTermina LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL3 REQUIRED CONFIG)

add_library(dot_runtime STATIC
    src/core/memory.cpp
    src/core/loader.cpp
    src/hle/svc.cpp
    src/hle/srv.cpp
    src/hle/fs.cpp
    src/hle/hid.cpp
    src/hle/dsp.cpp
)
target_include_directories(dot_runtime PUBLIC src)
target_link_libraries(dot_runtime PUBLIC SDL3::SDL3)
target_compile_options(dot_runtime PUBLIC -Wall -Wextra)

add_executable(dawnoftermina src/main.cpp)
target_link_libraries(dawnoftermina PRIVATE dot_runtime)

enable_testing()
add_executable(dot_selftest tests/selftest.cpp)
target_link_libraries(dot_selftest PRIVATE dot_runtime)
add_test(NAME selftest COMMAND dot_selftest ${CMAKE_SOURCE_DIR}/game_data)
