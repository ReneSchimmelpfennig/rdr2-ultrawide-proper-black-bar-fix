cmake_minimum_required(VERSION 3.25)

project(RDR2UltrawideCutsceneFix
    VERSION 0.1.0
    DESCRIPTION "Removes cutscene letterboxing while preserving the original framing"
    LANGUAGES CXX C)

if(NOT MSVC)
    message(FATAL_ERROR "This project targets MSVC only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RDR2 is x64 only -- configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: an ASI plugin is loaded into a process we do not control, so it
# must not depend on the VC redistributable being present.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# MinHook
#
# SOURCE_SUBDIR points at a directory that does not exist on purpose: it stops
# FetchContent from running add_subdirectory() on MinHook's own CMakeLists.txt,
# which still declares cmake_minimum_required(VERSION 3.0.2) and is therefore
# rejected outright by CMake 4.x. We only want the sources; the target below is
# ours, and inherits our CRT and warning settings.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
add_library(${PROJECT_NAME} SHARED
    src/dllmain.cpp
    src/bars.cpp
    src/bars.h
    src/dump.cpp
    src/dump.h
    src/fov.cpp
    src/fov.h
    src/hunt.cpp
    src/hunt.h
    src/watchpoint.cpp
    src/watchpoint.h
    src/log.cpp
    src/log.h
    src/mem.cpp
    src/mem.h
    src/patterns.h
    src/framing.h)

set_target_properties(${PROJECT_NAME} PROPERTIES
    SUFFIX ".asi"
    OUTPUT_NAME "RDR2UltrawideCutsceneFix")

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    PLUGIN_VERSION="${PROJECT_VERSION}")

target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)

target_link_libraries(${PROJECT_NAME} PRIVATE minhook version.lib)

# ---------------------------------------------------------------------------
# Self-test. The scanner runs inside a packed process where a wrong answer is
# either an access violation or a plausible-looking wrong address, so it gets
# exercised against buffers we control before it goes anywhere near RDR2.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(scanner_test
    tests/scanner_test.cpp src/mem.cpp src/log.cpp src/dump.cpp src/hunt.cpp)
target_compile_definitions(scanner_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(scanner_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(scanner_test PRIVATE version.lib)

add_test(NAME scanner COMMAND scanner_test)

# ---------------------------------------------------------------------------
# Optional deploy. Never runs unless RDR2_GAME_DIR is set explicitly:
#   cmake --build build --config Release --target deploy
# ---------------------------------------------------------------------------
set(RDR2_GAME_DIR "" CACHE PATH "RDR2 install directory; enables the 'deploy' target")

if(RDR2_GAME_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${PROJECT_NAME}> "${RDR2_GAME_DIR}"
        DEPENDS ${PROJECT_NAME}
        COMMENT "Copying $<TARGET_FILE_NAME:${PROJECT_NAME}> to ${RDR2_GAME_DIR}"
        VERBATIM)
endif()
