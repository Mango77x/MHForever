# Core: console-independent code (docs/02-architecture.md §1).
add_library(mhf_core STATIC
    base/version.cpp
)
target_include_directories(mhf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mhf_core PRIVATE MHF_VERSION_STRING="${PROJECT_VERSION}")
mhf_target_warnings(mhf_core)

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
