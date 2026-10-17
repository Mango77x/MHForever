# Freedom Unite module (docs/06-mhfu-module.md).

# Game file-system helpers used by developer tools (DATA.BIN container, overlay headers).
add_library(mhf_mhfu_data STATIC tools/data_bin.cpp)
target_include_directories(mhf_mhfu_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
mhf_target_warnings(mhf_mhfu_data)

# Developer-only: extracts code overlays from the developer's own ISO (docs/04-recompiler.md §4).
add_executable(mhfu_extract_overlays tools/extract_overlays.cpp)
target_link_libraries(mhfu_extract_overlays PRIVATE mhf_mhfu_data mhf_psp_tools psprecomp_core)
mhf_target_warnings(mhfu_extract_overlays)

add_library(mhf_game_mhfu INTERFACE)
target_link_libraries(mhf_game_mhfu INTERFACE mhf_psp mhf_mhfu_data)

if(MHF_BUILD_TESTS)
    add_executable(test_mhfu_data_bin tests/test_data_bin.cpp)
    target_include_directories(test_mhfu_data_bin PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_mhfu_data_bin PRIVATE mhf_mhfu_data)
    mhf_target_warnings(test_mhfu_data_bin)
    add_test(NAME mhfu_data_bin COMMAND test_mhfu_data_bin)
endif()

# The game itself, built from code recompiled on the developer's machine (never committed, see CONTRIBUTING.md).
# Prepare it with games/mhfu/tools/prepare.py, then configure with -DMHF_MHFU_GENERATED=<that folder>.
set(MHF_MHFU_GENERATED "" CACHE PATH "Folder with mhf_recomp output for Freedom Unite (developer-only)")
if(MHF_MHFU_GENERATED)
    file(GLOB mhfu_generated_sources CONFIGURE_DEPENDS ${MHF_MHFU_GENERATED}/*.cpp)
    # The generated code is its own library, compiled only against the stable ABI headers, so that host-side changes
    # (frontend, SDL, options) never force the long rebuild of the game code.
    add_library(mhfu_generated STATIC ${mhfu_generated_sources})
    target_include_directories(mhfu_generated PRIVATE ${MHF_MHFU_GENERATED})
    target_link_libraries(mhfu_generated PRIVATE mhf_psp_runtime)
    if(MSVC)
        target_compile_options(mhfu_generated PRIVATE /w /bigobj /O1)
    else()
        target_compile_options(mhfu_generated PRIVATE -w -O1)
    endif()
    add_executable(mhfu host/main.cpp)
    target_include_directories(mhfu PRIVATE ${MHF_MHFU_GENERATED})
    target_link_libraries(mhfu PRIVATE mhfu_generated mhf_psp_runtime)
    mhf_psp_copy_fonts(mhfu)
    if(TARGET mhf_psp_frontend)
        target_link_libraries(mhfu PRIVATE mhf_psp_frontend)
    endif()
    mhf_target_warnings(mhfu)
endif()
