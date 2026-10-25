# FFmpeg (LGPL version 2.1 or later) for the moving pictures of the game's movies (the H.264 of every PSMF) and for the
# ATRAC3/ATRAC3+ sound of the movies and the music of the game (hle_atrac.cpp). It is mandatory: without it the runtime
# cannot play movies or decode music at all.
#
# MHF_FFMPEG selects where it comes from:
#   system  (default on Linux)   The libavcodec and libavutil the system provides (pkg-config): the Linux player links
#                                against the distribution's FFmpeg, so the package manager keeps it patched and the
#                                repository stores no binary of it.
#   bundled (default on Windows) On Windows, a pinned prebuilt LGPL shared build is copied next to the executable. On
#                                Linux, it downloads the pinned release below, checks its SHA-256 and builds it at
#                                configure time with the LGPL-only configuration below, linked statically into the
#                                runtime, which is what a self-contained archive needs. A player needs no FFmpeg
#                                installed in either case.
#
# The project is GPL-3.0-or-later and re-licenses the LGPL code as GPL-3 in the way section 4 of the LGPL-2.1 allows.
# The licence and the source note live in third_party/ffmpeg/, and scripts/package_release.py ships them with the rest
# of the licences; the repository never stores a binary of the library.
#
# Defines mhf_ffmpeg and mhf_use_ffmpeg(<target>).

set(MHF_FFMPEG_VERSION 7.1.5)
set(MHF_FFMPEG_URL "https://ffmpeg.org/releases/ffmpeg-${MHF_FFMPEG_VERSION}.tar.xz")
set(MHF_FFMPEG_SHA256 de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f)

# Only the decoders the game needs: H.264 for the movie pictures and ATRAC3/ATRAC3+ for their sound and the game's
# music. No demuxer, parser, scaler or resampler: the runtime splits the movie stream into access units itself
# (media/psmf_demuxer.cpp) and converts pixels itself. No GPL or non-free parts; the build checks it.
set(MHF_FFMPEG_CONFIGURE_FLAGS
    --enable-static
    --disable-shared
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
    --enable-decoder=h264
    --enable-decoder=atrac3
    --enable-decoder=atrac3p
    --disable-x86asm
    --disable-debug)

# Windows x64: the LGPL shared build of the same release branch, kept by that project for two years. Built without
# --enable-gpl and --enable-nonfree, so it is LGPL-3.0-or-later. It also ships libswresample (not used here) as its own
# dependency.
set(MHF_FFMPEG_WINDOWS_URL
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-06-30-13-34/ffmpeg-n7.1.5-1-g7d0e842004-win64-lgpl-shared-7.1.zip")
set(MHF_FFMPEG_WINDOWS_SHA256 03a8003e245c08df4277d7b0adc50b93a97ddd4a3aaafea21943c4384df59895)
set(MHF_FFMPEG_WINDOWS_DLLS avcodec-61 avutil-59 swresample-5)

set(_MHF_FFMPEG_DEFAULT "system")
if(WIN32)
    set(_MHF_FFMPEG_DEFAULT "bundled")
endif()
# Linux takes the system's FFmpeg unless asked otherwise: the player links against the distribution's copy, so the
# package manager keeps it patched and the repository carries no binary of it. Windows takes the pinned archive, a
# prebuilt LGPL shared build that goes next to the executable. `bundled` is also there for a self-contained archive.
set(MHF_FFMPEG "${_MHF_FFMPEG_DEFAULT}" CACHE STRING
    "Where FFmpeg comes from: system (pkg-config) or bundled (downloaded, checked against its SHA-256 and built here)")
set_property(CACHE MHF_FFMPEG PROPERTY STRINGS bundled system)
string(TOUPPER "${MHF_FFMPEG}" _mhf_ffmpeg_mode)
if(_mhf_ffmpeg_mode MATCHES "^(ON|TRUE|YES|Y|1|BUNDLED)$")
    set(_mhf_ffmpeg_mode bundled)
elseif(_mhf_ffmpeg_mode STREQUAL "SYSTEM")
    set(_mhf_ffmpeg_mode system)
else()
    message(FATAL_ERROR "MHF_FFMPEG must be bundled or system, not '${MHF_FFMPEG}'")
endif()

set(MHF_FFMPEG_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/downloads" CACHE PATH
    "Where the bundled FFmpeg archive is downloaded to, or found without downloading")

# FFmpeg keeps its configure line, prefix included, as a string in the libraries. The bundled build is configured for a
# neutral prefix and installed through DESTDIR, so no path of the build machine ends up in the final code.
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

if(_mhf_ffmpeg_mode STREQUAL "system")
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(MHF_LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil)
    endif()
    if(NOT MHF_LIBAV_FOUND)
        message(FATAL_ERROR "MHF_FFMPEG=system, but pkg-config finds no libavcodec and libavutil. Install FFmpeg's development "
            "files (for example libavcodec-dev and libavutil-dev on Debian/Ubuntu, ffmpeg on Arch), or configure with "
            "-DMHF_FFMPEG=bundled to build the pinned release instead.")
    endif()
    add_library(mhf_ffmpeg INTERFACE)
    target_link_libraries(mhf_ffmpeg INTERFACE PkgConfig::MHF_LIBAV)
    message(STATUS "mhf: FFmpeg libavcodec ${MHF_LIBAV_libavcodec_VERSION} from the system (LGPL; the distribution's build)")

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
    # The DLLs are imported, so every executable that links mhf_ffmpeg loads them from its own folder: mhf_use_ffmpeg
    # copies them beside the target after it is linked (the game, and the tests that link the runtime).
    set(_mhf_ffmpeg_dll_files "")
    foreach(dll IN LISTS MHF_FFMPEG_WINDOWS_DLLS)
        set(_mhf_ffmpeg_from "${_mhf_ffmpeg_prefix}/bin/${dll}.dll")
        if(NOT EXISTS "${_mhf_ffmpeg_from}")
            # The archive said it was extracted (its stamp is there) but this file is not: extract it again rather than
            # carrying on with a build whose executables could not start.
            message(STATUS "mhf: ${_mhf_ffmpeg_from} is missing; extracting FFmpeg again")
            _mhf_ffmpeg_fetch(_mhf_ffmpeg_archive "${_mhf_ffmpeg_zip}" "${MHF_FFMPEG_WINDOWS_URL}" "${MHF_FFMPEG_WINDOWS_SHA256}")
            file(REMOVE_RECURSE "${_mhf_ffmpeg_prefix}")
            file(ARCHIVE_EXTRACT INPUT "${_mhf_ffmpeg_archive}" DESTINATION "${_mhf_ffmpeg_root}")
            file(WRITE "${_mhf_ffmpeg_prefix}/mhf.stamp" "${MHF_FFMPEG_WINDOWS_SHA256}\n")
            if(NOT EXISTS "${_mhf_ffmpeg_from}")
                message(FATAL_ERROR "mhf: the FFmpeg archive has no ${dll}.dll in bin/; the pin may be wrong")
            endif()
        endif()
        configure_file("${_mhf_ffmpeg_from}" "${_mhf_ffmpeg_bin}/${dll}.dll" COPYONLY)
        list(APPEND _mhf_ffmpeg_dll_files "${_mhf_ffmpeg_bin}/${dll}.dll")
    endforeach()
    set(MHF_FFMPEG_WINDOWS_DLL_FILES "${_mhf_ffmpeg_dll_files}" CACHE INTERNAL "FFmpeg DLLs to copy beside executables" FORCE)
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
    message(STATUS "mhf: bundled FFmpeg ${MHF_FFMPEG_VERSION} (prebuilt, LGPL, dynamic on Windows)")

else()
    find_program(MHF_MAKE NAMES gmake make)
    if(NOT MHF_MAKE)
        message(FATAL_ERROR "mhf: building the bundled FFmpeg needs make. Install it, or configure with -DMHF_FFMPEG=system.")
    endif()
    set(_mhf_ffmpeg_src "${_mhf_ffmpeg_root}/ffmpeg-${MHF_FFMPEG_VERSION}")
    set(_mhf_ffmpeg_prefix "${_mhf_ffmpeg_root}/install")
    set(_mhf_ffmpeg_neutral_prefix /ffmpeg)
    set(_mhf_ffmpeg_flags ${MHF_FFMPEG_CONFIGURE_FLAGS})
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

    # Static libraries linked into every user of mhf_ffmpeg.
    add_library(mhf_ffmpeg INTERFACE)
    foreach(lib avcodec avutil)
        add_library(mhf_ffmpeg_${lib} STATIC IMPORTED)
        set_target_properties(mhf_ffmpeg_${lib} PROPERTIES IMPORTED_LOCATION "${_mhf_ffmpeg_prefix}/lib/lib${lib}.a")
        target_link_libraries(mhf_ffmpeg INTERFACE mhf_ffmpeg_${lib})
    endforeach()
    target_include_directories(mhf_ffmpeg INTERFACE "${_mhf_ffmpeg_prefix}/include")
    message(STATUS "mhf: bundled FFmpeg ${MHF_FFMPEG_VERSION} (LGPL, h264+atrac3+atrac3p, static)")
endif()

# Links <target> against FFmpeg.
function(mhf_use_ffmpeg target)
    target_link_libraries(${target} PRIVATE mhf_ffmpeg)
endfunction()

# Copies FFmpeg's DLLs beside every executable that ends up linking them, once all the targets exist (call it at the end
# of the top-level CMakeLists.txt). On Windows the modules are imported, and an executable loads them from its own
# folder: the game needs them next to itself, and so do the tests that link the runtime (psp_hle_io, psp_hle_savedata),
# which run from their own directories. Following the links matters because a target may reach FFmpeg through the
# runtime library rather than by calling mhf_use_ffmpeg() itself.
function(mhf_ffmpeg_copy_dlls)
    # The list of DLLs is a cache entry so that it survives between configurations; an empty one (a configuration that
    # ran before the DLLs were known, or a stale cache) must not silently turn the copying off, so it is asked for again
    # when a print of it is missing instead of being trusted as is.
    if(NOT WIN32)
        return()
    endif()
    if(NOT MHF_FFMPEG_WINDOWS_DLL_FILES)
        message(WARNING "mhf: the FFmpeg DLLs are unknown in this build directory; configure again so that they end up "
            "beside the executables, or those executables will not start on Windows")
        return()
    endif()
    # A DLL that is not there would make every executable fail to start with STATUS_DLL_NOT_FOUND, and the failure would
    # look like a linking problem; the build says which one is missing instead.
    foreach(dll IN LISTS MHF_FFMPEG_WINDOWS_DLL_FILES)
        if(NOT EXISTS "${dll}")
            message(FATAL_ERROR "mhf: ${dll} is missing; configure again so that FFmpeg is fetched and extracted")
        endif()
    endforeach()
    # A target reaches FFmpeg if it links mhf_ffmpeg or links a target that reaches it: the game calls mhf_use_ffmpeg()
    # through the runtime library, and the tests link the runtime, so following the links is what finds them.
    #
    # The targets have to be collected from every directory: BUILDSYSTEM_TARGETS only lists the ones defined in the
    # directory it is asked about, so asking the top-level one alone would miss everything a subdirectory creates
    # (the game, the tests, the launcher), which is exactly what leaves executables without their DLLs.
    set(all_targets "")
    set(_mhf_dirs "${PROJECT_SOURCE_DIR}")
    while(_mhf_dirs)
        list(POP_FRONT _mhf_dirs dir)
        get_property(_mhf_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        if(_mhf_targets)
            list(APPEND all_targets ${_mhf_targets})
        endif()
        get_property(_mhf_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        if(_mhf_subdirs)
            list(APPEND _mhf_dirs ${_mhf_subdirs})
        endif()
    endwhile()
    list(REMOVE_DUPLICATES all_targets)
    set(reaches "")
    set(seen "")
    set(pending ${all_targets})
    while(pending)
        list(POP_FRONT pending target)
        if(NOT target OR target IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${target})
        get_target_property(links ${target} LINK_LIBRARIES)
        get_target_property(iface ${target} INTERFACE_LINK_LIBRARIES)
        foreach(list_name links iface)
            if(NOT ${list_name})
                continue()
            endif()
            foreach(link IN LISTS ${list_name})
                # A STATIC library carries a PRIVATE dependency as $<LINK_ONLY:name> in its interface, so the name has to
                # be unwrapped before it can be compared with mhf_ffmpeg or asked about with TARGET(): otherwise a target
                # that reaches FFmpeg only through the runtime (which is how the game and its tests get it) is missed.
                string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" link "${link}")
                if(link STREQUAL "mhf_ffmpeg")
                    list(APPEND reaches ${target})
                elseif(TARGET ${link})
                    list(APPEND pending ${link})
                endif()
            endforeach()
        endforeach()
    endwhile()
    # Propagate along the links (the list only grows, so the walk terminates even where libraries link each other).
    set(changed TRUE)
    while(changed)
        set(changed FALSE)
        foreach(target IN LISTS all_targets)
            if(target IN_LIST reaches)
                continue()
            endif()
            get_target_property(links ${target} LINK_LIBRARIES)
            get_target_property(iface ${target} INTERFACE_LINK_LIBRARIES)
            foreach(list_name links iface)
                if(NOT ${list_name})
                    continue()
                endif()
                foreach(link IN LISTS ${list_name})
                    string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" link "${link}") # see the walk above
                    if(link IN_LIST reaches)
                        list(APPEND reaches ${target})
                        set(changed TRUE)
                        break()
                    endif()
                endforeach()
                if(target IN_LIST reaches)
                    break()
                endif()
            endforeach()
        endforeach()
    endwhile()
    if(NOT reaches)
        return()
    endif()
    # add_custom_command(TARGET ...) only accepts a target created in the directory the call is made from, and these
    # targets come from all over the tree. A target of its own carries the copying for each executable instead: it runs
    # after the executable is built, uses the executable's own directory, and can be declared from anywhere.
    set(copied "")
    foreach(target IN LISTS reaches)
        get_target_property(kind ${target} TYPE)
        if(kind STREQUAL "EXECUTABLE")
            add_custom_target(${target}_ffmpeg_dlls ALL
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MHF_FFMPEG_WINDOWS_DLL_FILES} $<TARGET_FILE_DIR:${target}>
                COMMENT "Copying FFmpeg's DLLs next to ${target}"
                VERBATIM)
            add_dependencies(${target}_ffmpeg_dlls ${target})
            list(APPEND copied ${target})
        endif()
    endforeach()
    if(copied)
        list(JOIN copied ", " copied)
        message(STATUS "mhf: FFmpeg's DLLs go beside ${copied}")
    endif()
endfunction()
