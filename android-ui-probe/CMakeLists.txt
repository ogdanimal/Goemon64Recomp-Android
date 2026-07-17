# Throwaway probe: prove the UI stack (freetype -> RmlUi -> lunasvg) cross-compiles
# for arm64-v8a under the NDK. This is the last M3 link unknown, since Windows uses
# prebuilt freetype and Linux uses system freetype -- neither exists on Android.
# Mirrors the root CMake's RmlUi/lunasvg wiring. Discard once folded into root.
cmake_minimum_required(VERSION 3.20)
project(GoemonAndroidUiProbe LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)

include(FetchContent)
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

# --- FreeType (Android has no system freetype; RmlUi needs the Freetype::Freetype target) ---
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG VER-2-13-3
    GIT_SHALLOW TRUE
)
# FreeType's optional compression/font deps aren't needed for RmlUi glyph rendering.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(freetype)
# Modern FreeType exports `freetype` but not always the `Freetype::Freetype` alias RmlUi expects.
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# --- lunasvg (pure C++, static) ---
set(BUILD_SHARED_LIBS OFF)
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT}/lib/lunasvg ${CMAKE_BINARY_DIR}/lunasvg)

# --- RmlUi (freetype font engine + svg plugin) ---
set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
add_subdirectory(${REPO_ROOT}/lib/RmlUi ${CMAKE_BINARY_DIR}/RmlUi)
target_compile_definitions(rmlui_core PRIVATE LUNASVG_BUILD_STATIC)
