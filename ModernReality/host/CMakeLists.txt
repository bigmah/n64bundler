# SPDX-License-Identifier: GPL-3.0-or-later
#
# n64b-run - the host that runs a recompiled game module.
#
# Built once and shared by every game in the library. RT64, librecomp and
# ultramodern are all in here; a game is a small dylib this opens. That is the
# whole reason the split exists -- linking the renderer into each title would
# give a library of twenty games twenty copies of it.

find_package(SDL2 REQUIRED)

# RT64 comes in static.
#
# It builds a dylib by default, which would relink faster, but it is compiled
# with hidden visibility and exposes no C API: its application object is
# reached through its own headers. Nothing in a dylib built that way is
# callable from outside it. Static, the same symbols resolve at link time and
# the visibility only stops them being re-exported, which is what we want
# anyway -- a game module has no business calling into the renderer.
set(RT64_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64" rt64)

add_executable(n64b-run
    src/main.cpp
    src/module.cpp
    src/renderer.cpp
    src/window.cpp
    src/audio.cpp
    src/input.cpp
    src/callbacks.cpp
    src/hooks.cpp
)

target_include_directories(n64b-run PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    # RT64 has no installed header set: its application object is reached
    # through the source tree, the way its own frontends reach it.
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src/contrib"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src/contrib/plume"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src/contrib/hlslpp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src/contrib/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/thirdparty"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/thirdparty/sse2neon"
    # librecomp does not export where recomp.h lives, so name them here.
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/N64Recomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/librecomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/ultramodern/include"
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(n64b-run PRIVATE
    modernreality_abi
    librecomp
    ultramodern
    rt64
    ${SDL2_LIBRARIES}
)

# RT64's headers are shared with its shaders: the same file is compiled as
# HLSL on the GPU and as C++ here, and HLSL_CPU is what picks the C++ half.
target_compile_definitions(n64b-run PRIVATE HLSL_CPU FFX_GCC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

target_compile_options(n64b-run PRIVATE -Wall -Wno-unused-parameter -Wno-unused-private-field)

# Why the link line looks like this.
#
# A game module's undefined symbols -- every libultra call it makes, every
# runtime helper the recompiled code uses -- are bound against this executable
# when it is opened. Two things have to be true for that to work.
#
# -export_dynamic puts the host's symbols in its dynamic table, so dyld can
# find them at all. Without it the module opens and every call into the runtime
# is a null pointer.
#
# -force_load pulls every object out of the runtime archives even though the
# host itself references none of them. `osCreateThread_recomp` is called only
# from a game that does not exist yet at link time, so without this the linker
# is entirely right to leave it out, and the module fails to load with a
# missing symbol naming a function that was built an hour ago.
target_link_options(n64b-run PRIVATE
    -Wl,-export_dynamic
    "-Wl,-force_load,$<TARGET_FILE:librecomp>"
    "-Wl,-force_load,$<TARGET_FILE:ultramodern>"
    "-Wl,-force_load,$<TARGET_FILE:modernreality_ultra_gaps>"
)
add_dependencies(n64b-run modernreality_ultra_gaps)
