# SPDX-License-Identifier: GPL-3.0-or-later
#
# ModernReality - the runtime and the tools around it.
#
# Three things get built here:
#
#   n64rip     recovers sections and functions from a bare ROM, so a game with
#              no decompilation behind it still has metadata to recompile
#   n64b-run   the host: opens a recompiled game module and runs it over
#              librecomp, ultramodern and RT64
#   the upstream recompiler binaries, which the pipeline drives directly
#
# Everything upstream comes in through vendor/, and librecomp pulls N64Recomp
# in behind it, so adding N64ModernRuntime here is enough to get rabbitizer,
# fmt, the recompiler library and the CLI tools.

cmake_minimum_required(VERSION 3.20)
project(ModernReality CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Everything the game module and the host agree on lives here.
add_library(modernreality_abi INTERFACE)
target_include_directories(modernreality_abi INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_subdirectory(vendor/N64ModernRuntime)

# The recompiler CLIs come in EXCLUDE_FROM_ALL through librecomp, which is the
# right default for a project that only links the library. The pipeline runs
# them as programs, so they have to be built.
foreach(tool N64RecompCLI RSPRecomp)
    if(TARGET ${tool})
        set_target_properties(${tool} PROPERTIES EXCLUDE_FROM_ALL FALSE)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The libultra helpers librecomp does not carry. Built here rather than with the
# host because build.sh reads the symbol list out of it to decide which names
# the analyser is allowed to use, and that has to work before the host exists.

add_library(modernreality_ultra_gaps STATIC src/ultra_gaps.cpp)
target_link_libraries(modernreality_ultra_gaps PRIVATE librecomp ultramodern)
target_include_directories(modernreality_ultra_gaps PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/N64Recomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/librecomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/ultramodern/include"
)

# ---------------------------------------------------------------------------
# n64sig - fingerprinting libultra, so its functions can be named in a ROM.
# Shared as a library because the analyser matches against what this builds.

add_library(n64sig_core STATIC
    tools/n64sig/signature.cpp
    tools/n64sig/harvest.cpp
)
target_include_directories(n64sig_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tools/n64sig")
target_compile_options(n64sig_core PRIVATE -Wall -Wno-unused-parameter)

add_executable(n64sig tools/n64sig/main.cpp)
target_link_libraries(n64sig PRIVATE n64sig_core)
target_compile_options(n64sig PRIVATE -Wall -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# n64rip

add_executable(n64rip
    tools/n64rip/main.cpp
    tools/n64rip/rom.cpp
    tools/n64rip/analyze.cpp
    tools/n64rip/emit.cpp
    tools/n64rip/title.cpp
)
target_link_libraries(n64rip PRIVATE rabbitizer n64sig_core)
# xxHash for the ROM hash the runtime validates against, and toml++ for the
# title records. Both come in header-only, from the recompiler's own vendoring.
target_include_directories(n64rip PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/thirdparty"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/N64Recomp/lib/tomlplusplus/include"
)
target_compile_options(n64rip PRIVATE -Wall -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# n64b-port - the recompiler and the compiler, driven.
#
# Where its siblings live is baked in rather than looked up, because the app
# runs it from ~/Applications with launchd's PATH and no idea where the checkout
# is. build.sh writes the same paths into toolchain.conf for the same reason.

set(RECOMP_BIN_DIR "${CMAKE_BINARY_DIR}/vendor/N64ModernRuntime/librecomp/N64Recomp")

set(MR_MODULE_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/include|${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/N64Recomp/include|${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/librecomp/include|${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/ultramodern/include|${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/thirdparty|${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/thirdparty/sse2neon")

add_executable(n64b-port tools/n64b-port/main.cpp)
target_include_directories(n64b-port PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/N64ModernRuntime/thirdparty"
)
target_compile_definitions(n64b-port PRIVATE
    N64B_DEFAULT_RECOMP="${RECOMP_BIN_DIR}/N64Recomp"
    N64B_DEFAULT_RSP_RECOMP="${RECOMP_BIN_DIR}/RSPRecomp"
    # What the recompiled C includes: recomp.h, the runtime's headers, and this
    # tree's own module ABI.
    # Separated by | rather than the obvious : or ; -- a semicolon is CMake's
    # own list separator and would split the definition into several.
    N64B_DEFAULT_INCLUDES="${MR_MODULE_INCLUDES}"
)
target_compile_options(n64b-port PRIVATE -Wall -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# n64b-run
#
# Added once the host exists; RT64 is a long build and there is no reason to
# pay for it while only the analyser is being worked on. -DMODERNREALITY_HOST=ON
# turns it on.

option(MODERNREALITY_HOST "Build the n64b-run host (pulls in RT64)" OFF)
if(MODERNREALITY_HOST)
    add_subdirectory(host)
endif()
