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
