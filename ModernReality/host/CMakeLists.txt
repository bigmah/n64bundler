# SPDX-License-Identifier: GPL-3.0-or-later
#
# n64b-run - the host that runs a recompiled game module.
#
# Built once and shared by every game in the library. RT64, librecomp and
# ultramodern are all in here; a game is a small shared library this opens.
# That is the whole reason the split exists -- linking the renderer into each
# title would give a library of twenty games twenty copies of it.

find_package(SDL2 REQUIRED)

if(MODERNREALITY_RENDERER)
    # RT64 comes in static.
    #
    # It builds a shared library by default, which would relink faster, but it
    # is compiled with hidden visibility and exposes no C API: its application
    # object is reached through its own headers. Nothing in a shared library
    # built that way is callable from outside it. Static, the same symbols
    # resolve at link time and the visibility only stops them being
    # re-exported, which is what we want anyway -- a game module has no
    # business calling into the renderer.
    set(RT64_STATIC ON CACHE BOOL "" FORCE)
    # Outside Windows and macOS the renderer draws through Vulkan, onto a window
    # SDL made. RT64 calls that RT64_SDL_WINDOW_VULKAN and it is off by default,
    # because its own frontend on Windows uses D3D12 on a native window.
    if(NOT WIN32 AND NOT APPLE)
        set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64" rt64)
endif()

add_executable(n64b-run
    src/main.cpp
    src/module.cpp
    src/audio.cpp
    src/input.cpp
    src/callbacks.cpp
    src/hooks.cpp
    src/overlays.cpp
    src/trace.cpp
    src/gym.cpp
)

# The two files that are the renderer. Everything else here runs a game whether
# or not anybody is going to see it.
if(MODERNREALITY_RENDERER)
    target_sources(n64b-run PRIVATE src/renderer.cpp src/window.cpp)
else()
    # What the rest of the host still expects to find without them.
    target_sources(n64b-run PRIVATE src/no_renderer.cpp)
endif()

target_include_directories(n64b-run PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/thirdparty"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/thirdparty/sse2neon"
    # librecomp does not export where recomp.h lives, so name them here.
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/N64Recomp/include"
    # The recompiler's own headers reach for rabbitizer, which is vendored
    # inside it and not exported either. overlays.cpp runs the recompiler while
    # a game is running, which is what needs them.
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/N64Recomp/lib/rabbitizer/cplusplus/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/N64Recomp/lib/rabbitizer/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/librecomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/N64ModernRuntime/ultramodern/include"
    ${SDL2_INCLUDE_DIRS}
)

if(MODERNREALITY_RENDERER)
    # RT64 has no installed header set: its application object is reached
    # through the source tree, the way its own frontends reach it.
    target_include_directories(n64b-run PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src/contrib"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src/contrib/plume"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src/contrib/hlslpp/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/rt64/src/contrib/imgui"
    )
endif()

target_link_libraries(n64b-run PRIVATE
    modernreality_abi
    librecomp
    # The recompiler, for the code a game decompresses into memory it
    # allocated: see host/src/overlays.cpp.
    N64Recomp
    LiveRecomp
    ultramodern
    # Also force-loaded below. Named here as well so that the link depends on
    # it: without that, adding a libultra helper to it leaves the host linked
    # against yesterday's copy and a game fails to open with a symbol that was
    # built a minute ago.
    modernreality_ultra_gaps
    ${SDL2_LIBRARIES}
)

if(MODERNREALITY_RENDERER)
    target_link_libraries(n64b-run PRIVATE rt64)
    if(APPLE)
        # For a picture of the window itself: the window server numbers the
        # window and hands its contents back, which is the only screenshot that
        # is what the game looks like. See host/src/window.cpp. There is no
        # equivalent anywhere else, and nothing but a screenshot needs it.
        target_link_libraries(n64b-run PRIVATE "-framework ApplicationServices")
    endif()
endif()

# RT64's headers are shared with its shaders: the same file is compiled as
# HLSL on the GPU and as C++ here, and HLSL_CPU is what picks the C++ half.
if(MODERNREALITY_RENDERER)
    target_compile_definitions(n64b-run PRIVATE HLSL_CPU FFX_GCC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
endif()

target_compile_options(n64b-run PRIVATE -Wall -Wno-unused-parameter -Wno-unused-private-field)

# Why the link line looks like this.
#
# A game module's undefined symbols -- every libultra call it makes, every
# runtime helper the recompiled code uses -- are bound against this executable
# when it is opened. Two things have to be true for that to work.
#
# Exporting dynamically puts the host's symbols in its dynamic table, so the
# loader can find them at all. Without it the module opens and every call into
# the runtime is a null pointer.
#
# Force-loading pulls every object out of the runtime archives even though the
# host itself references none of them. `osCreateThread_recomp` is called only
# from a game that does not exist yet at link time, so without this the linker
# is entirely right to leave it out, and the module fails to load with a
# missing symbol naming a function that was built an hour ago.
#
# Apple's linker and the GNU one spell both of these differently, and neither
# accepts the other's spelling, so this is said twice.
if(APPLE)
    target_link_options(n64b-run PRIVATE
        -Wl,-export_dynamic
        "-Wl,-force_load,$<TARGET_FILE:librecomp>"
        "-Wl,-force_load,$<TARGET_FILE:ultramodern>"
        "-Wl,-force_load,$<TARGET_FILE:modernreality_ultra_gaps>"
    )
else()
    # --whole-archive is a mode rather than an argument: it applies to every
    # archive after it until --no-whole-archive turns it off again, so each one
    # is wrapped rather than named.
    target_link_options(n64b-run PRIVATE
        -Wl,--export-dynamic
        "-Wl,--whole-archive,$<TARGET_FILE:librecomp>,--no-whole-archive"
        "-Wl,--whole-archive,$<TARGET_FILE:ultramodern>,--no-whole-archive"
        "-Wl,--whole-archive,$<TARGET_FILE:modernreality_ultra_gaps>,--no-whole-archive"
    )
endif()
