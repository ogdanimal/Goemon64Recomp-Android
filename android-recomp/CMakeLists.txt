# Standalone CMake project to cross-compile just the recompiler-output static
# libraries (RecompiledFuncs + PatchesLib) for arm64-v8a under the Android NDK.
#
# This is the M2 arm64 proof: the generated recompiled C compiles clean for the
# target. The full app link (RT64 + N64ModernRuntime + SDL + these into a single
# libGoemon64.so) is M3. Target definitions mirror the root CMakeLists.txt so the
# two stay in sync; REPO_ROOT points one level up at the recompiler output.
cmake_minimum_required(VERSION 3.20)
project(GoemonAndroidRecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)

set(RECOMP_INCLUDES
    ${REPO_ROOT}/include
    ${REPO_ROOT}/lib/N64ModernRuntime/ultramodern/include
    ${REPO_ROOT}/lib/N64ModernRuntime/librecomp/include
    ${REPO_ROOT}/lib/N64ModernRuntime/N64Recomp/include
)

# --- RecompiledFuncs (primary recompiler output) ---
add_library(RecompiledFuncs STATIC)
target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)
target_include_directories(RecompiledFuncs PRIVATE ${RECOMP_INCLUDES})
file(GLOB FUNC_C_SOURCES ${REPO_ROOT}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${REPO_ROOT}/RecompiledFuncs/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

# --- PatchesLib (recompiled custom-patch output) ---
add_library(PatchesLib STATIC)
target_compile_options(PatchesLib PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)
target_include_directories(PatchesLib PRIVATE ${RECOMP_INCLUDES})
target_sources(PatchesLib PRIVATE
    ${REPO_ROOT}/RecompiledPatches/patches.c
    ${REPO_ROOT}/RecompiledPatches/patches_bin.c
)
set_source_files_properties(${REPO_ROOT}/RecompiledPatches/patches.c
    PROPERTIES COMPILE_FLAGS -fno-strict-aliasing)
