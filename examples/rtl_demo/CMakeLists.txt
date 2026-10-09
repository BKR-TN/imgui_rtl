cmake_minimum_required(VERSION 3.20)
project(imgui_rtl C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(BUILD_SHARED_LIBS OFF)

# This file lives in examples/rtl_demo/, next to the demo sources and fonts; the dear imgui
# checkout it builds is two levels up.
get_filename_component(IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

option(RTL_BUILD_DEMO "Build the GLFW/OpenGL3 RTL demo" ON)
option(RTL_BUILD_SELFTEST "Build the headless RTL self-test" ON)
option(RTL_BUILD_BENCHMARK "Build the headless RTL benchmark" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
#
# HarfBuzz (shaping), SheenBidi (bidi) and libraqm (complex text layout) are used
# from a local checkout when one is available (vendored next to this file, in the
# imgui checkout, or as a sibling directory of it), and otherwise fetched
# automatically with FetchContent (pinned to the versions below). FreeType and
# GLFW are system dependencies resolved via find_package/pkg-config.
# ---------------------------------------------------------------------------

# FreeType (system dependency; needed by libraqm and by imgui's freetype loader).
find_package(Freetype REQUIRED)

# Directories searched for local dependency checkouts, in order.
set(RTL_DEP_SEARCH_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"   # vendored next to this CMakeLists.txt
    "${IMGUI_ROOT}"                 # vendored in the dear imgui checkout
    "${IMGUI_ROOT}/.."              # sibling checkout (typical dev layout)
)

# --- HarfBuzz (shaping engine; needs FreeType interop) ---
set(HB_BUILD_SUBSET       OFF CACHE BOOL "" FORCE)
set(HB_BUILD_RASTER       OFF CACHE BOOL "" FORCE)
set(HB_BUILD_VECTOR       OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU          OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS        OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE      ON  CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB          OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU           OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GRAPHITE2     OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GOBJECT       OFF CACHE BOOL "" FORCE)
set(HB_HAVE_INTROSPECTION OFF CACHE BOOL "" FORCE)

set(RTL_HARFBUZZ_DIR "")
foreach(dir ${RTL_DEP_SEARCH_DIRS})
    if(RTL_HARFBUZZ_DIR STREQUAL "" AND EXISTS "${dir}/harfbuzz/CMakeLists.txt")
        set(RTL_HARFBUZZ_DIR "${dir}/harfbuzz")
    endif()
endforeach()
if(NOT RTL_HARFBUZZ_DIR STREQUAL "")
    add_subdirectory("${RTL_HARFBUZZ_DIR}" harfbuzz)
else()
    FetchContent_Declare(harfbuzz
        GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG        14.5.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(harfbuzz)
endif()

# --- SheenBidi (Unicode bidirectional algorithm) ---
set(SB_CONFIG_EXPERIMENTAL_TEXT_API OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_COVERAGE OFF CACHE BOOL "" FORCE)
set(ENABLE_ASAN OFF CACHE BOOL "" FORCE)
set(ENABLE_UBSAN OFF CACHE BOOL "" FORCE)

set(RTL_SHEENBIDI_DIR "")
foreach(dir ${RTL_DEP_SEARCH_DIRS})
    if(RTL_SHEENBIDI_DIR STREQUAL "" AND EXISTS "${dir}/SheenBidi/CMakeLists.txt")
        set(RTL_SHEENBIDI_DIR "${dir}/SheenBidi")
    endif()
endforeach()
if(NOT RTL_SHEENBIDI_DIR STREQUAL "")
    add_subdirectory("${RTL_SHEENBIDI_DIR}" SheenBidi)
else()
    FetchContent_Declare(SheenBidi
        GIT_REPOSITORY https://github.com/Tehreer/SheenBidi.git
        GIT_TAG        v3.0.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(SheenBidi)
endif()

# --- libraqm (complex text layout; meson-only, effectively a single translation unit) ---
set(RAQM_SOURCE_DIR "")
foreach(dir ${RTL_DEP_SEARCH_DIRS})
    if(RAQM_SOURCE_DIR STREQUAL "" AND EXISTS "${dir}/libraqm/src/raqm.c")
        set(RAQM_SOURCE_DIR "${dir}/libraqm")
    endif()
endforeach()
if(RAQM_SOURCE_DIR STREQUAL "")
    # libraqm has no CMakeLists.txt (meson-only), so we fetch its source and compile
    # raqm.c directly. FetchContent_Populate() is the documented way to fetch a source
    # tree without configuring it.
    cmake_policy(SET CMP0169 OLD)
    FetchContent_Declare(libraqm
        GIT_REPOSITORY https://github.com/HOST-Oman/libraqm.git
        GIT_TAG        v0.11.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_Populate(libraqm)
    set(RAQM_SOURCE_DIR "${libraqm_SOURCE_DIR}")
endif()

# libraqm generates raqm-version.h from its meson project version. Read it from the checkout we
# actually use, so a local checkout of a different version doesn't get misreported as the pinned one.
file(READ "${RAQM_SOURCE_DIR}/meson.build" RAQM_MESON_BUILD)
if(RAQM_MESON_BUILD MATCHES "version[ \t]*:[ \t]*'([0-9]+\\.[0-9]+\\.[0-9]+)'")
    set(RAQM_VERSION "${CMAKE_MATCH_1}")
else()
    set(RAQM_VERSION "0.11.0") # fallback: the version pinned by FetchContent below
    message(WARNING "imgui RTL: could not parse libraqm version from ${RAQM_SOURCE_DIR}/meson.build; assuming ${RAQM_VERSION}")
endif()
string(REPLACE "." ";" RAQM_VERSION_LIST "${RAQM_VERSION}")
list(GET RAQM_VERSION_LIST 0 RAQM_VERSION_MAJOR)
list(GET RAQM_VERSION_LIST 1 RAQM_VERSION_MINOR)
list(GET RAQM_VERSION_LIST 2 RAQM_VERSION_MICRO)
message(STATUS "imgui RTL: libraqm ${RAQM_VERSION} from '${RAQM_SOURCE_DIR}'")

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/raqm-gen)
configure_file(
    ${RAQM_SOURCE_DIR}/src/raqm-version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/raqm-gen/raqm-version.h
    @ONLY
)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/raqm-gen/config.h
"#ifndef RAQM_CONFIG_H\n#define RAQM_CONFIG_H\n#define RAQM_SHEENBIDI 1\n#endif\n"
)

add_library(raqm STATIC ${RAQM_SOURCE_DIR}/src/raqm.c)
target_include_directories(raqm PUBLIC
    ${RAQM_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/raqm-gen
)
target_compile_definitions(raqm PUBLIC HAVE_CONFIG_H RAQM_SHEENBIDI_GT_2_9)
target_link_libraries(raqm PUBLIC Freetype::Freetype harfbuzz SheenBidi)

# ---------------------------------------------------------------------------
# Dear ImGui library (core + FreeType glyph loader + RTL shaper)
# ---------------------------------------------------------------------------
add_library(imgui STATIC
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/imgui_demo.cpp
    ${IMGUI_ROOT}/misc/freetype/imgui_freetype.cpp
    ${IMGUI_ROOT}/misc/rtl/imgui_rtl.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_ROOT}
    ${IMGUI_ROOT}/misc/freetype
    ${IMGUI_ROOT}/misc/rtl
    ${IMGUI_ROOT}/backends
)
target_compile_definitions(imgui PUBLIC
    IMGUI_ENABLE_FREETYPE
    IMGUI_ENABLE_RTL
)
target_link_libraries(imgui PUBLIC Freetype::Freetype raqm)

# ---------------------------------------------------------------------------
# Targets (default font location baked in so the binaries work from the build dir)
# ---------------------------------------------------------------------------
set(RTL_DEMO_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fonts")

# Headless self-test (no window/backend)
if(RTL_BUILD_SELFTEST)
    add_executable(rtl_selftest selftest.cpp)
    target_link_libraries(rtl_selftest PRIVATE imgui)
    target_compile_definitions(rtl_selftest PRIVATE RTL_DEMO_FONT_DIR="${RTL_DEMO_FONT_DIR}")

    # Register with CTest so `ctest` (or `ctest --output-on-failure`) runs the suite.
    enable_testing()
    add_test(NAME rtl_selftest COMMAND rtl_selftest)
endif()

# Headless benchmark (RTL shaping vs LTR)
if(RTL_BUILD_BENCHMARK)
    add_executable(rtl_benchmark benchmark.cpp)
    target_link_libraries(rtl_benchmark PRIVATE imgui)
    target_compile_definitions(rtl_benchmark PRIVATE RTL_DEMO_FONT_DIR="${RTL_DEMO_FONT_DIR}")
endif()

# Demo application (GLFW + OpenGL3)
if(RTL_BUILD_DEMO)
    find_package(OpenGL REQUIRED)
    find_package(glfw3 3.3 QUIET)
    if(NOT glfw3_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
    endif()

    add_executable(rtl_demo
        main.cpp
        ${IMGUI_ROOT}/backends/imgui_impl_glfw.cpp
        ${IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp
    )
    target_link_libraries(rtl_demo PRIVATE imgui OpenGL::GL)
    target_compile_definitions(rtl_demo PRIVATE RTL_DEMO_FONT_DIR="${RTL_DEMO_FONT_DIR}")
    if(TARGET glfw)
        target_link_libraries(rtl_demo PRIVATE glfw)
    elseif(TARGET glfw3::glfw3)
        target_link_libraries(rtl_demo PRIVATE glfw3::glfw3)
    elseif(TARGET PkgConfig::GLFW)
        target_link_libraries(rtl_demo PRIVATE PkgConfig::GLFW)
    endif()
endif()

message(STATUS "imgui RTL: root='${IMGUI_ROOT}' fonts='${RTL_DEMO_FONT_DIR}'")
