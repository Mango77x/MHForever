# Core: console-independent code (docs/02-architecture.md §1).
add_library(mhf_core STATIC
    base/version.cpp
    host/keymap.cpp
    host/padmap.cpp
)
target_include_directories(mhf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mhf_core PRIVATE MHF_VERSION_STRING="${PROJECT_VERSION}")
mhf_target_warnings(mhf_core)

# Rooms shared by every console's multiplayer adapter (docs/08-multiplayer.md §1.1).
add_library(mhf_net STATIC
    net/local_network.cpp
)
target_include_directories(mhf_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mhf_target_warnings(mhf_net)

# Overlays drawn over a game: text and shapes (Painter) and the in-game menu (docs/07-launcher-ux.md section 3c).
add_library(mhf_overlay STATIC overlay/painter.cpp overlay/menu.cpp overlay/icon.cpp)
target_include_directories(mhf_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mhf_overlay PRIVATE mhf_stb_truetype)
mhf_target_warnings(mhf_overlay)

# The local link between the game program and the launcher (docs/07-launcher-ux.md section 3c).
add_library(mhf_link STATIC link/link.cpp link/protocol.cpp)
target_include_directories(mhf_link PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(mhf_link PUBLIC ws2_32)
endif()
mhf_target_warnings(mhf_link)

if(MHF_BUILD_TESTS)
    add_executable(test_link tests/test_link.cpp)
    target_include_directories(test_link PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_link PRIVATE mhf_link)
    mhf_target_warnings(test_link)
    add_test(NAME link_local COMMAND test_link)

    add_executable(test_menu tests/test_menu.cpp)
    target_include_directories(test_menu PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_compile_definitions(test_menu PRIVATE MHF_FONT_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts/liberation-sans")
    target_link_libraries(test_menu PRIVATE mhf_overlay)
    mhf_target_warnings(test_menu)
    add_test(NAME overlay_menu COMMAND test_menu)

    add_executable(test_link_protocol tests/test_link_protocol.cpp)
    target_include_directories(test_link_protocol PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_link_protocol PRIVATE mhf_link)
    mhf_target_warnings(test_link_protocol)
    add_test(NAME link_protocol COMMAND test_link_protocol)

    add_executable(test_local_network tests/test_local_network.cpp)
    target_include_directories(test_local_network PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_local_network PRIVATE mhf_net)
    mhf_target_warnings(test_local_network)
    add_test(NAME net_local_network COMMAND test_local_network)
endif()

# Launcher entry point. For now it only reports its version; see docs/07-launcher-ux.md.
add_executable(mhf app/main.cpp)
target_link_libraries(mhf PRIVATE mhf_core)
mhf_target_warnings(mhf)

# Desktop host (window, input, audio) on SDL3.
if(MHF_WITH_SDL)
    include(${PROJECT_SOURCE_DIR}/cmake/sdl3.cmake)
    add_library(mhf_core_host STATIC host/sdl_host.cpp host/keymap_sdl.cpp host/padmap_sdl.cpp)
    target_include_directories(mhf_core_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mhf_core_host PUBLIC SDL3::SDL3-static mhf_core)
    mhf_target_warnings(mhf_core_host)

    if(MHF_BUILD_TESTS)
        add_executable(test_keymap tests/test_keymap.cpp)
        target_include_directories(test_keymap PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
        target_link_libraries(test_keymap PRIVATE mhf_core_host)
        mhf_target_warnings(test_keymap)
        add_test(NAME host_keymap COMMAND test_keymap)

        add_executable(test_padmap tests/test_padmap.cpp)
        target_include_directories(test_padmap PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
        target_link_libraries(test_padmap PRIVATE mhf_core_host)
        mhf_target_warnings(test_padmap)
        add_test(NAME host_padmap COMMAND test_padmap)
    endif()
endif()
