# SDL3 (zlib license) for window, input and audio (docs/02-architecture.md §5). Built from source as a static library.
include(FetchContent)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# The archive is fetched first with retries (cmake/download.cmake), as GitHub release downloads fail now and then.
include(${CMAKE_CURRENT_LIST_DIR}/download.cmake)
set(_sdl3_url https://github.com/libsdl-org/SDL/releases/download/release-3.2.24/SDL3-3.2.24.tar.gz)
set(_sdl3_sha256 81cc0fc17e5bf2c1754eeca9af9c47a76789ac5efdd165b3b91cbbe4b90bfb76)
set(_sdl3_archive "${CMAKE_BINARY_DIR}/_deps/downloads/SDL3-3.2.24.tar.gz")
if(NOT FETCHCONTENT_SOURCE_DIR_SDL3 AND NOT FETCHCONTENT_FULLY_DISCONNECTED)
    mhf_download("${_sdl3_url}" "${_sdl3_archive}" "${_sdl3_sha256}" _sdl3_problem)
    if(_sdl3_problem)
        message(FATAL_ERROR "mhf: ${_sdl3_problem}")
    endif()
endif()
FetchContent_Declare(SDL3
    URL "${_sdl3_archive}"
    URL_HASH SHA256=${_sdl3_sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL3)
