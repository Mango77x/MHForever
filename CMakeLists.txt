cmake_minimum_required(VERSION 3.20)
project(MHForever VERSION 0.8.0 LANGUAGES C CXX)

# Windows executables carry the MHForever icon (a resource file, compiled by the platform resource compiler).
if(WIN32)
    enable_language(RC)
    set(MHF_WINDOWS_ICON_RC ${CMAKE_SOURCE_DIR}/assets/branding/MHForever.rc)
endif()

# Release programs must run on a clean Windows install: link the C/C++ runtime statically (no VC++ redistributable).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MHF_BUILD_TESTS "Build tests" ON)
option(MHF_WITH_SDL "Build the SDL3 desktop host (window, input, audio)" ON)
option(MHF_BUILD_LAUNCHER "Build the launcher (needs Node.js to build its UI; WebKitGTK on Linux)" ON)
option(MHF_WARNINGS_AS_ERRORS "Treat warnings in our own code as errors" OFF)
option(MHF_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
set(MHF_GAMES "mhfu" CACHE STRING "Semicolon-separated list of game modules to build")

# Floating-point semantics must match across platforms (docs/04-recompiler.md §9).
if(MSVC)
    add_compile_options(/fp:precise /utf-8)
else()
    add_compile_options(-ffp-contract=off -fno-fast-math)
endif()

if(MHF_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "MHF_SANITIZE needs GCC or Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Compiler warnings for our own targets (not applied to third_party).
function(mhf_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(MHF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(MHF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(MHF_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party/psprecomp)
add_subdirectory(third_party/stb)
add_subdirectory(third_party/basisu)
add_subdirectory(third_party/pokke_save_crypto)
# FFmpeg decodes the movies (H.264 + ATRAC3/ATRAC3+) and the game's music; by default it is built with the project
# (cmake/ffmpeg.cmake, docs/02-architecture.md section 9).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ffmpeg.cmake)
# The Japanese font of sceLibFont, downloaded and checked (cmake/fonts.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fonts.cmake)
add_subdirectory(third_party/aemu_postoffice)
add_subdirectory(core)
add_subdirectory(platforms/psp)
foreach(game IN LISTS MHF_GAMES)
    add_subdirectory(games/${game})
endforeach()
if(MHF_BUILD_LAUNCHER)
    add_subdirectory(launcher)
endif()
if(MHF_BUILD_TESTS)
    add_subdirectory(homebrew)
    add_subdirectory(tests)
endif()
# After every target exists: on Windows, FFmpeg's DLLs go beside each executable that links them (the tests included).
mhf_ffmpeg_copy_dlls()
