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
