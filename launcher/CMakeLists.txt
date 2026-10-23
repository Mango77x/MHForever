# The launcher: the player-facing hub (docs/07-launcher-ux.md). A native host (native/) shows the web UI (ui/) in the
# system web view and does the real work: identifying the player's copy, settings, launching the game, play time.

include(${PROJECT_SOURCE_DIR}/cmake/webview.cmake)
include(GNUInstallDirs)

# ------------------------------------------------------------------ UI bundle (one self-contained HTML file)
set(MHF_LAUNCHER_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ui)
set(_ui_html ${MHF_LAUNCHER_UI_DIR}/dist/index.html)
file(GLOB_RECURSE _ui_sources CONFIGURE_DEPENDS ${MHF_LAUNCHER_UI_DIR}/src/*)
find_program(MHF_NPM NAMES npm npm.cmd)
if(MHF_NPM)
    add_custom_command(
        OUTPUT ${_ui_html}
        COMMAND ${MHF_NPM} ci --no-audit --no-fund
        COMMAND ${MHF_NPM} run build
        WORKING_DIRECTORY ${MHF_LAUNCHER_UI_DIR}
        DEPENDS ${_ui_sources} ${MHF_LAUNCHER_UI_DIR}/package.json ${MHF_LAUNCHER_UI_DIR}/package-lock.json
                ${MHF_LAUNCHER_UI_DIR}/index.html ${MHF_LAUNCHER_UI_DIR}/vite.config.ts
        COMMENT "Building the launcher UI (npm)"
        VERBATIM)
elseif(NOT EXISTS ${_ui_html})
    message(FATAL_ERROR "The launcher UI needs Node.js/npm to build (or a prebuilt ${_ui_html}).")
endif()
set(_ui_cpp ${CMAKE_CURRENT_BINARY_DIR}/ui_bundle.cpp)
add_custom_command(
    OUTPUT ${_ui_cpp}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_ui_html} -DOUTPUT=${_ui_cpp} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS ${_ui_html} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    COMMENT "Embedding the launcher UI"
    VERBATIM)

# Linux: embed the application icon so the window and the task bar show it without installing any file next to the program.
set(_icon_cpp "")
if(UNIX)
    set(_icon_png ${PROJECT_SOURCE_DIR}/assets/branding/icon-256.png)
    set(_icon_cpp ${CMAKE_CURRENT_BINARY_DIR}/app_icon.cpp)
    add_custom_command(
        OUTPUT ${_icon_cpp}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_icon_png} -DOUTPUT=${_icon_cpp} -DSYMBOL=kAppIconPng
                "-DCOMMENT=assets/branding/icon-256.png" -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
        DEPENDS ${_icon_png} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
        COMMENT "Embedding the application icon"
        VERBATIM)
endif()

# ------------------------------------------------------------------ game catalog (from games/*/game.toml and regions)
# Identification data only (disc ids, dump sizes and SHA-1 hashes), which the legal rules allow (see CONTRIBUTING.md).
set(_catalog ${CMAKE_CURRENT_BINARY_DIR}/catalog.inc)
set(_catalog_text "")
foreach(game IN LISTS MHF_GAMES)
    set(_game_dir ${PROJECT_SOURCE_DIR}/games/${game})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_game_dir}/game.toml)
    file(STRINGS ${_game_dir}/game.toml _lines REGEX "^short_title")
    string(REGEX REPLACE "^short_title *= *\"([^\"]*)\".*" "\\1" _title "${_lines}")
    file(GLOB _regions LIST_DIRECTORIES true ${_game_dir}/regions/*)
    foreach(region_dir IN LISTS _regions)
        if(NOT EXISTS ${region_dir}/region.toml)
            continue()
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${region_dir}/region.toml)
        file(STRINGS ${region_dir}/region.toml _lines)
        set(_section "")
        set(_disc "")
        set(_version "")
        set(_market "")
        set(_status "")
        set(_dumps "")
        set(_dump_size "")
        set(_exe_size "0")
        set(_exe_sha1 "")
        set(_enc_size "0")
        set(_enc_sha1 "")
        foreach(line IN LISTS _lines)
            if(line MATCHES "^\\[\\[?([a-z_]+)\\]")
                set(_section "${CMAKE_MATCH_1}")
            elseif(line MATCHES "^([a-z0-9_]+) *= *\"?([^\"#]*)\"?")
                set(_key "${CMAKE_MATCH_1}")
                string(STRIP "${CMAKE_MATCH_2}" _value)
                if(_section STREQUAL "")
                    if(_key STREQUAL "disc_id")
                        set(_disc "${_value}")
                    elseif(_key STREQUAL "disc_version")
                        set(_version "${_value}")
                    elseif(_key STREQUAL "market")
                        set(_market "${_value}")
                    elseif(_key STREQUAL "status")
                        set(_status "${_value}")
                    endif()
                elseif(_section STREQUAL "accepted_dumps")
                    if(_key STREQUAL "size")
                        set(_dump_size "${_value}")
                    elseif(_key STREQUAL "sha1")
                        string(APPEND _dumps "{${_dump_size}ULL, \"${_value}\"}, ")
                    endif()
                elseif(_section STREQUAL "encrypted_executable")
                    if(_key STREQUAL "size")
                        set(_enc_size "${_value}")
                    elseif(_key STREQUAL "sha1")
                        set(_enc_sha1 "${_value}")
                    endif()
                elseif(_section STREQUAL "dev_executable")
                    if(_key STREQUAL "size")
                        set(_exe_size "${_value}")
                    elseif(_key STREQUAL "sha1")
                        set(_exe_sha1 "${_value}")
                    endif()
                endif()
            endif()
        endforeach()
        string(APPEND _catalog_text
            "{\"${game}\", \"${_title}\", \"${_disc}\", \"${_version}\", \"${_market}\", \"${_status}\", {${_dumps}}, ${_exe_size}ULL, \"${_exe_sha1}\", ${_enc_size}ULL, \"${_enc_sha1}\"},\n")
    endforeach()
endforeach()
file(CONFIGURE OUTPUT ${_catalog} CONTENT "${_catalog_text}")

# ------------------------------------------------------------------ executable
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/ppsspp_prx ${CMAKE_BINARY_DIR}/third_party/ppsspp_prx)

add_executable(mhf_launcher
    native/main.cpp
    native/launcher.cpp
    native/json.cpp
    native/hash.cpp
    native/platform.cpp
    native/install/data_dir.cpp
    native/install/executable_preparation.cpp
    native/install/installer.cpp
    native/install/prx_decryptor.cpp
    ${_ui_cpp}
    ${_icon_cpp}
)
target_include_directories(mhf_launcher PRIVATE native ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(mhf_launcher PRIVATE mhf_core mhf_psp_tools mhf_webview mhf_prx)
target_compile_definitions(mhf_launcher PRIVATE MHF_VERSION_STRING="${PROJECT_VERSION}")
set_target_properties(mhf_launcher PROPERTIES OUTPUT_NAME MHForever) # MHForever.exe on Windows
if(WIN32)
    set_target_properties(mhf_launcher PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(mhf_launcher PRIVATE ${MHF_WINDOWS_ICON_RC})
    target_link_libraries(mhf_launcher PRIVATE comdlg32 dwmapi bcrypt shell32 ole32)
    if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_options(mhf_launcher PRIVATE -mwindows)
    endif()
endif()
mhf_target_warnings(mhf_launcher)

# The launcher starts each game's program from its own folder, so building the launcher also builds every game program
# that exists (a game target exists only when its generated code is configured) and places it (with its fonts) next to the launcher.
foreach(game IN LISTS MHF_GAMES)
    if(TARGET ${game})
        add_dependencies(mhf_launcher ${game})
        add_custom_command(TARGET mhf_launcher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${game}> $<TARGET_FILE_DIR:mhf_launcher>
            COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:${game}>/fonts $<TARGET_FILE_DIR:mhf_launcher>/fonts
            COMMENT "Placing ${game} and its fonts next to the launcher"
            VERBATIM)
    endif()
endforeach()

# ------------------------------------------------------------------ install (Linux)
# `cmake --install build/linux-clang --prefix ~/.local` puts the launcher, the game (when it was built), the fonts,
# the desktop entry and the icon where the desktop finds them.
if(UNIX AND NOT APPLE)
    install(TARGETS mhf_launcher RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    foreach(game IN LISTS MHF_GAMES)
        if(TARGET ${game})
            install(TARGETS ${game} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
            set(_installs_games ON)
        endif()
    endforeach()
    if(_installs_games)
        install(DIRECTORY ${PROJECT_SOURCE_DIR}/third_party/fonts/liberation-sans/
                DESTINATION ${CMAKE_INSTALL_BINDIR}/fonts
                FILES_MATCHING PATTERN "*.ttf" PATTERN "LICENSE")
    endif()
    install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/MHForever.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(size 16 32 64 128 256 512)
        install(FILES ${PROJECT_SOURCE_DIR}/assets/branding/icon-${size}.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME mhforever.png)
    endforeach()
    install(FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/THIRD_PARTY.md
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/MHForever)
endif()

# ------------------------------------------------------------------ tests
if(MHF_BUILD_TESTS)
    add_executable(test_launcher tests/test_launcher.cpp native/launcher.cpp native/json.cpp native/hash.cpp
                                 native/platform.cpp native/install/data_dir.cpp
                                 native/install/executable_preparation.cpp native/install/installer.cpp
                                 native/install/prx_decryptor.cpp ${_icon_cpp})
    target_include_directories(test_launcher PRIVATE native ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(test_launcher PRIVATE mhf_psp_tools mhf_webview mhf_prx)
    if(WIN32)
        target_link_libraries(test_launcher PRIVATE comdlg32 dwmapi bcrypt shell32 ole32) # platform.cpp, data_dir.cpp
    endif()
    target_compile_definitions(test_launcher PRIVATE MHF_VERSION_STRING="${PROJECT_VERSION}")
    add_test(NAME launcher_core COMMAND test_launcher)

    # The installer on a synthetic disc image and a fake decryptor: no game data.
    add_executable(test_install tests/test_install.cpp native/install/data_dir.cpp native/install/executable_preparation.cpp
                                native/install/installer.cpp native/install/prx_decryptor.cpp native/hash.cpp native/json.cpp)
    target_include_directories(test_install PRIVATE native)
    target_link_libraries(test_install PRIVATE mhf_psp_tools mhf_prx)
    if(WIN32)
        target_link_libraries(test_install PRIVATE shell32 ole32) # data_dir.cpp
    endif()
    mhf_target_warnings(test_install)
    add_test(NAME launcher_install COMMAND test_install)
endif()
