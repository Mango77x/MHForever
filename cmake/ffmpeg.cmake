# FFmpeg (LGPL version 3 or later) for the game's movies and music: the H.264 decoder of an avc stream and the ATRAC3+
# audio that comes with it. Only libavcodec and libavutil are used, always as shared libraries (docs/02-architecture.md
# section 9): the project is GPL-3.0-or-later and links them dynamically, and the build with FFmpeg is the shipped one.
#
# MHF_FFMPEG selects where they come from:
#   bundled (default) On macOS and Linux, download the pinned release below, check its SHA-256, and build it at configure
#                     time with the LGPL-only configuration below. On Windows, download a pinned prebuilt LGPL shared
#                     build with it. Either way the libraries end up next to the executable (lib/), so a player needs no
#                     FFmpeg installed.
#   system            The libavcodec and libavutil that pkg-config finds (handy while developing).
#   OFF               No FFmpeg: movies are skipped and ATRAC3+ through FFmpeg is off, as before (tests, minimal builds).
#
# Sets MHF_HAS_FFMPEG and defines mhf_use_ffmpeg(<target>).

set(MHF_FFMPEG_VERSION 7.1.5)
set(MHF_FFMPEG_URL "https://ffmpeg.org/releases/ffmpeg-${MHF_FFMPEG_VERSION}.tar.xz")
set(MHF_FFMPEG_SHA256 de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f)
set(MHF_FFMPEG_SOVERSIONS avcodec 61 avutil 59)

# Only the decoders the game needs. No demuxer, parser, scaler or resampler: the runtime splits the movie stream into
# access units itself (media/movie_stream.cpp) and converts pixels itself. No GPL or non-free parts; the build checks it.
set(MHF_FFMPEG_CONFIGURE_FLAGS
    --enable-shared
    --disable-static
    --disable-programs
    --disable-doc
    --disable-avdevice
    --disable-avformat
    --disable-avfilter
    --disable-swscale
    --disable-swresample
    --disable-network
    --disable-autodetect
    --disable-everything
    --enable-decoder=atrac3,atrac3p,h264
    --disable-x86asm
    --disable-debug)

# Windows x64: the LGPL shared build of the same release branch, kept by that project for two years. Built without --enable-gpl
# and --enable-nonfree, so it is LGPL-3.0-or-later. It also ships libswresample (not used here) as its own dependency.
set(MHF_FFMPEG_WINDOWS_URL
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-06-30-13-34/ffmpeg-n7.1.5-1-g7d0e842004-win64-lgpl-shared-7.1.zip")
set(MHF_FFMPEG_WINDOWS_SHA256 03a8003e245c08df4277d7b0adc50b93a97ddd4a3aaafea21943c4384df59895)
set(MHF_FFMPEG_WINDOWS_DLLS avcodec-61 avutil-59 swresample-5)

set(MHF_FFMPEG "bundled" CACHE STRING "Where FFmpeg comes from: bundled (built with the project), system (pkg-config) or OFF")
set_property(CACHE MHF_FFMPEG PROPERTY STRINGS bundled system OFF)
string(TOUPPER "${MHF_FFMPEG}" _mhf_ffmpeg_mode)
if(_mhf_ffmpeg_mode MATCHES "^(ON|TRUE|YES|Y|1|BUNDLED)$")
    set(_mhf_ffmpeg_mode bundled)
elseif(_mhf_ffmpeg_mode MATCHES "^(OFF|FALSE|NO|N|0)$")
    set(_mhf_ffmpeg_mode OFF)
elseif(_mhf_ffmpeg_mode STREQUAL "SYSTEM")
    set(_mhf_ffmpeg_mode system)
else()
    message(FATAL_ERROR "MHF_FFMPEG must be bundled, system or OFF, not '${MHF_FFMPEG}'")
endif()

set(MHF_FFMPEG_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/downloads" CACHE PATH
    "Where the bundled FFmpeg archive is downloaded to, or found without downloading")

set(MHF_HAS_FFMPEG OFF)
set(_mhf_ffmpeg_root "${CMAKE_BINARY_DIR}/_deps/ffmpeg")
set(_mhf_ffmpeg_bin "${CMAKE_BINARY_DIR}/bin")

# Downloads <url> to <file name> under MHF_FFMPEG_DOWNLOAD_DIR unless a copy with the pinned SHA-256 is already there;
# sets <out> to its path.
function(_mhf_ffmpeg_fetch out name url sha256)
    set(archive "${MHF_FFMPEG_DOWNLOAD_DIR}/${name}")
    if(EXISTS "${archive}")
        file(SHA256 "${archive}" existing)
        if(existing STREQUAL sha256)
            set(${out} "${archive}" PARENT_SCOPE)
            return()
        endif()
    endif()
    message(STATUS "mhf: downloading ${url}")
    file(DOWNLOAD "${url}" "${archive}.part" STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${archive}.part")
        message(FATAL_ERROR "mhf: could not download ${url}: ${status}\n"
            "Put the file in ${MHF_FFMPEG_DOWNLOAD_DIR} by hand, or configure with -DMHF_FFMPEG=system to use an installed FFmpeg.")
    endif()
    file(SHA256 "${archive}.part" actual)
    if(NOT actual STREQUAL sha256)
        file(REMOVE "${archive}.part")
        message(FATAL_ERROR "mhf: ${url} has SHA-256 ${actual}, expected ${sha256}")
    endif()
    file(RENAME "${archive}.part" "${archive}")
    set(${out} "${archive}" PARENT_SCOPE)
endfunction()

# Runs a step of the FFmpeg build with its output in <log>; stops with the end of the log on failure.
function(_mhf_ffmpeg_run what log)
    execute_process(COMMAND ${ARGN}
        WORKING_DIRECTORY "${_mhf_ffmpeg_root}/build"
        OUTPUT_FILE "${log}" ERROR_FILE "${log}.err"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        file(READ "${log}.err" errors)
        message(FATAL_ERROR "mhf: FFmpeg ${what} failed (${result}); see ${log} and ${log}.err\n${errors}")
    endif()
endfunction()

# A short note next to the libraries: what they are and where their source is.
function(_mhf_ffmpeg_write_notice dir)
    string(CONCAT text ${ARGN})
    file(WRITE "${dir}/FFmpeg-SOURCE.txt" "${text}")
endfunction()

if(_mhf_ffmpeg_mode STREQUAL "OFF")
    message(STATUS "mhf: FFmpeg off; movies are skipped (MHF_FFMPEG=OFF)")

elseif(_mhf_ffmpeg_mode STREQUAL "system")
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(MHF_LIBAV QUIET IMPORTED_TARGET libavcodec libavutil)
    endif()
    if(NOT MHF_LIBAV_FOUND)
        message(FATAL_ERROR "MHF_FFMPEG=system, but pkg-config finds no libavcodec and libavutil. Install FFmpeg's development "
            "files, or configure with -DMHF_FFMPEG=bundled (the default).")
    endif()
    add_library(mhf_ffmpeg INTERFACE)
    target_link_libraries(mhf_ffmpeg INTERFACE PkgConfig::MHF_LIBAV)
    set(MHF_HAS_FFMPEG ON)
    message(STATUS "mhf: FFmpeg libavcodec ${MHF_LIBAV_libavcodec_VERSION} from the system; movies enabled")

elseif(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|arm|aarch")
        message(FATAL_ERROR "mhf: the bundled FFmpeg for Windows is x64 only; configure with -DMHF_FFMPEG=system.")
    endif()
    string(REGEX MATCH "[^/]+\\.zip$" _mhf_ffmpeg_zip "${MHF_FFMPEG_WINDOWS_URL}")
    string(REGEX REPLACE "\\.zip$" "" _mhf_ffmpeg_name "${_mhf_ffmpeg_zip}")
    set(_mhf_ffmpeg_prefix "${_mhf_ffmpeg_root}/${_mhf_ffmpeg_name}")
    if(NOT EXISTS "${_mhf_ffmpeg_prefix}/mhf.stamp")
        _mhf_ffmpeg_fetch(_mhf_ffmpeg_archive "${_mhf_ffmpeg_zip}" "${MHF_FFMPEG_WINDOWS_URL}" "${MHF_FFMPEG_WINDOWS_SHA256}")
        file(REMOVE_RECURSE "${_mhf_ffmpeg_prefix}")
        file(ARCHIVE_EXTRACT INPUT "${_mhf_ffmpeg_archive}" DESTINATION "${_mhf_ffmpeg_root}")
        file(WRITE "${_mhf_ffmpeg_prefix}/mhf.stamp" "${MHF_FFMPEG_WINDOWS_SHA256}\n")
    endif()
    foreach(dll IN LISTS MHF_FFMPEG_WINDOWS_DLLS)
        configure_file("${_mhf_ffmpeg_prefix}/bin/${dll}.dll" "${_mhf_ffmpeg_bin}/${dll}.dll" COPYONLY)
    endforeach()
    configure_file("${_mhf_ffmpeg_prefix}/LICENSE.txt" "${_mhf_ffmpeg_bin}/FFmpeg-LICENSE.txt" COPYONLY)
    _mhf_ffmpeg_write_notice("${_mhf_ffmpeg_bin}"
        "The FFmpeg libraries here (avcodec, avutil, swresample) are an unmodified prebuilt\n"
        "LGPL shared build of FFmpeg ${MHF_FFMPEG_VERSION} from BtbN/FFmpeg-Builds, licensed under the\n"
        "GNU LGPL version 3 or later (FFmpeg-LICENSE.txt). They are dynamically linked.\n\n"
        "Build:  ${MHF_FFMPEG_WINDOWS_URL}\n"
        "Source: https://github.com/FFmpeg/FFmpeg/tree/release/7.1 (the commit is in the file name)\n"
        "Build scripts: https://github.com/BtbN/FFmpeg-Builds\n")
    add_library(mhf_ffmpeg INTERFACE)
    foreach(lib avcodec avutil)
        add_library(mhf_ffmpeg_${lib} SHARED IMPORTED)
        foreach(dll IN LISTS MHF_FFMPEG_WINDOWS_DLLS)
            if(dll MATCHES "^${lib}-")
                set(_mhf_ffmpeg_dll "${dll}")
            endif()
        endforeach()
        set_target_properties(mhf_ffmpeg_${lib} PROPERTIES
            IMPORTED_LOCATION "${_mhf_ffmpeg_bin}/${_mhf_ffmpeg_dll}.dll"
            IMPORTED_IMPLIB "${_mhf_ffmpeg_prefix}/lib/${lib}.lib")
        target_link_libraries(mhf_ffmpeg INTERFACE mhf_ffmpeg_${lib})
    endforeach()
    target_include_directories(mhf_ffmpeg INTERFACE "${_mhf_ffmpeg_prefix}/include")
    set(MHF_HAS_FFMPEG ON)
    message(STATUS "mhf: bundled FFmpeg ${MHF_FFMPEG_VERSION} (prebuilt, LGPL); movies enabled")

else()
    find_program(MHF_MAKE NAMES gmake make)
    if(NOT MHF_MAKE)
        message(FATAL_ERROR "mhf: building the bundled FFmpeg needs make. Install it, or configure with -DMHF_FFMPEG=system.")
    endif()
    set(_mhf_ffmpeg_src "${_mhf_ffmpeg_root}/ffmpeg-${MHF_FFMPEG_VERSION}")
    set(_mhf_ffmpeg_prefix "${_mhf_ffmpeg_root}/install")
    # FFmpeg keeps its configure line, prefix included, as a string in the libraries. It is configured for a neutral prefix
    # and installed through DESTDIR, so no path of the build machine ends up in a shipped library.
    set(_mhf_ffmpeg_neutral_prefix /ffmpeg)
    set(_mhf_ffmpeg_flags ${MHF_FFMPEG_CONFIGURE_FLAGS})
    if(APPLE)
        list(APPEND _mhf_ffmpeg_flags --install-name-dir=@rpath)
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            list(APPEND _mhf_ffmpeg_flags
                "--extra-cflags=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
                "--extra-ldflags=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
    endif()
    if(DEFINED ENV{CC})
        list(APPEND _mhf_ffmpeg_flags "--cc=$ENV{CC}")
    endif()
    # Built once per build directory; again only when the pin or the flags change.
    string(REPLACE ";" " " _mhf_ffmpeg_stamp
        "${MHF_FFMPEG_VERSION} ${MHF_FFMPEG_SHA256} --prefix=${_mhf_ffmpeg_neutral_prefix} ${_mhf_ffmpeg_flags}")
    set(_mhf_ffmpeg_stamp_file "${_mhf_ffmpeg_prefix}/mhf.stamp")
    set(_mhf_ffmpeg_built "")
    if(EXISTS "${_mhf_ffmpeg_stamp_file}")
        file(READ "${_mhf_ffmpeg_stamp_file}" _mhf_ffmpeg_built)
    endif()
    if(NOT _mhf_ffmpeg_built STREQUAL _mhf_ffmpeg_stamp)
        _mhf_ffmpeg_fetch(_mhf_ffmpeg_archive "ffmpeg-${MHF_FFMPEG_VERSION}.tar.xz" "${MHF_FFMPEG_URL}" "${MHF_FFMPEG_SHA256}")
        message(STATUS "mhf: building FFmpeg ${MHF_FFMPEG_VERSION} (once per build directory)")
        file(REMOVE_RECURSE "${_mhf_ffmpeg_src}" "${_mhf_ffmpeg_root}/build" "${_mhf_ffmpeg_prefix}")
        file(ARCHIVE_EXTRACT INPUT "${_mhf_ffmpeg_archive}" DESTINATION "${_mhf_ffmpeg_root}")
        file(MAKE_DIRECTORY "${_mhf_ffmpeg_root}/build")
        set(_log "${_mhf_ffmpeg_root}/build")
        _mhf_ffmpeg_run(configure "${_log}/configure.log"
            sh "${_mhf_ffmpeg_src}/configure" "--prefix=${_mhf_ffmpeg_neutral_prefix}" ${_mhf_ffmpeg_flags})
        # The licensing in THIRD_PARTY.md assumes exactly this.
        file(READ "${_log}/configure.log" _mhf_ffmpeg_configure)
        file(READ "${_log}/config.h" _mhf_ffmpeg_config)
        if(NOT _mhf_ffmpeg_configure MATCHES "(^|\n)License: LGPL version 2\\.1 or later\n"
           OR NOT _mhf_ffmpeg_config MATCHES "\n#define CONFIG_GPL 0\n"
           OR NOT _mhf_ffmpeg_config MATCHES "\n#define CONFIG_NONFREE 0\n")
            message(FATAL_ERROR "mhf: the FFmpeg configuration is not LGPL-2.1-or-later only; see ${_log}/configure.log")
        endif()
        _mhf_ffmpeg_run(build "${_log}/build.log" "${MHF_MAKE}" -j)
        file(REMOVE_RECURSE "${_mhf_ffmpeg_root}/destdir")
        _mhf_ffmpeg_run(install "${_log}/install.log" "${MHF_MAKE}" install "DESTDIR=${_mhf_ffmpeg_root}/destdir")
        file(RENAME "${_mhf_ffmpeg_root}/destdir${_mhf_ffmpeg_neutral_prefix}" "${_mhf_ffmpeg_prefix}")
        file(REMOVE_RECURSE "${_mhf_ffmpeg_root}/destdir")
        file(WRITE "${_mhf_ffmpeg_stamp_file}" "${_mhf_ffmpeg_stamp}")
    endif()

    # The libraries go to lib/ next to the executable, under the names the loader looks for; the executable's rpath
    # points there (mhf_use_ffmpeg).
    set(_mhf_ffmpeg_lib "${_mhf_ffmpeg_bin}/lib")
    add_library(mhf_ffmpeg INTERFACE)
    set(_versions ${MHF_FFMPEG_SOVERSIONS})
    while(_versions)
        list(POP_FRONT _versions lib soversion)
        if(APPLE)
            set(runtime_name "lib${lib}.${soversion}.dylib")
        else()
            set(runtime_name "lib${lib}.so.${soversion}")
        endif()
        configure_file("${_mhf_ffmpeg_prefix}/lib/${runtime_name}" "${_mhf_ffmpeg_lib}/${runtime_name}" COPYONLY)
        add_library(mhf_ffmpeg_${lib} SHARED IMPORTED)
        set_target_properties(mhf_ffmpeg_${lib} PROPERTIES IMPORTED_LOCATION "${_mhf_ffmpeg_lib}/${runtime_name}")
        if(NOT APPLE)
            set_target_properties(mhf_ffmpeg_${lib} PROPERTIES IMPORTED_SONAME "${runtime_name}")
        endif()
        target_link_libraries(mhf_ffmpeg INTERFACE mhf_ffmpeg_${lib})
    endwhile()
    target_include_directories(mhf_ffmpeg INTERFACE "${_mhf_ffmpeg_prefix}/include")
    configure_file("${_mhf_ffmpeg_src}/COPYING.LGPLv2.1" "${_mhf_ffmpeg_lib}/FFmpeg-COPYING.LGPLv2.1.txt" COPYONLY)
    string(REPLACE ";" " " _mhf_ffmpeg_flag_text "${MHF_FFMPEG_CONFIGURE_FLAGS}")
    _mhf_ffmpeg_write_notice("${_mhf_ffmpeg_lib}"
        "The FFmpeg libraries here (libavcodec, libavutil) are FFmpeg ${MHF_FFMPEG_VERSION},\n"
        "unmodified, licensed under the GNU LGPL version 2.1 or later\n"
        "(FFmpeg-COPYING.LGPLv2.1.txt). They are dynamically linked.\n\n"
        "Source: ${MHF_FFMPEG_URL}\n"
        "SHA-256: ${MHF_FFMPEG_SHA256}\n"
        "Configured with: ${_mhf_ffmpeg_flag_text}\n")
    set(MHF_FFMPEG_RPATH "$ORIGIN/lib")
    if(APPLE)
        set(MHF_FFMPEG_RPATH "@loader_path/lib")
    endif()
    set(MHF_HAS_FFMPEG ON)
    message(STATUS "mhf: bundled FFmpeg ${MHF_FFMPEG_VERSION} (LGPL, h264 atrac3 atrac3p); movies enabled")
endif()

# Links <target> against FFmpeg and lets it find the bundled libraries.
function(mhf_use_ffmpeg target)
    if(NOT MHF_HAS_FFMPEG)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE MHF_HAS_FFMPEG=1)
    target_link_libraries(${target} PRIVATE mhf_ffmpeg)
    if(MHF_FFMPEG_RPATH)
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${MHF_FFMPEG_RPATH}")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "${MHF_FFMPEG_RPATH}")
    endif()
endfunction()
