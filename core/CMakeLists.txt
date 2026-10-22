# Core: console-independent code (docs/02-architecture.md §1).
add_library(mhf_core STATIC
    base/version.cpp
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

if(MHF_BUILD_TESTS)
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
    add_library(mhf_core_host STATIC host/sdl_host.cpp)
    target_include_directories(mhf_core_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mhf_core_host PUBLIC SDL3::SDL3-static)
    mhf_target_warnings(mhf_core_host)
endif()
