# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are forbidden; use a preset (cmake --preset default) or -B build")
endif()

project(blind-sight-lib
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Gaze-steered synthetic-echo sensory substitution — acoustic scene library and demo")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Use ccache if available (fast incremental rebuilds).
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache enabled")
endif()

# Compiler Options Hardening Guide for C and C++
# (https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html)
# Carried over from ~/spaces/gnome-agent-proxy's CMakeLists.txt, split into
# compile- vs. link-time flags (add_compile_options() passes its whole
# argument list to every compile invocation, so a linker-only -Wl,... flag
# has no business there even though the compiler driver silently tolerates
# it during -c).
add_compile_options(
    -Wall
    -Wformat
    -Wformat=2
    -Wconversion
    -Wimplicit-fallthrough
    -Wformat-security
    -Werror=format-security
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=3
    -D_GLIBCXX_ASSERTIONS
    -fstrict-flex-arrays=3
    -fstack-clash-protection
    -fstack-protector-strong
    -Wtrampolines
    -Wbidi-chars=any
    -fcf-protection=full
    -O2
)
add_link_options(
    -Wl,-z,noexecstack
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,--as-needed
)
# NOT carried over: -Werror. The reference project is a small, fully
# first-party daemon; this one links gtkmm/libadwaita/glib headers we don't
# control, and -Wconversion + -Werror against third-party headers is a real
# risk of an unbuildable tree on a toolchain bump we didn't cause. Revisit
# once the library side (our own code, no GTK headers) has its own target.

# -march=native is unsafe for a binary meant to run on a machine other than
# the one that built it — opt-in only, same reasoning as gnome-agent-proxy.
option(BLIND_SIGHT_MARCH_NATIVE
       "Compile for the build machine's exact CPU (-march=native); unsafe for distributed builds" OFF)
if(BLIND_SIGHT_MARCH_NATIVE)
    add_compile_options(-march=native)
endif()

# lib/ — the acoustic-scene library. Added once it has sources; the demo does
# not yet link it (first milestone is display-only, see notes.md).
# add_subdirectory(lib)

add_subdirectory(demo)
