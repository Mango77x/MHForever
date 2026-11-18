# Freedom Unite module (docs/06-mhfu-module.md).

# Game file-system helpers for local assets and developer tools (DATA.BIN container, overlay headers).
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
    # The runtime scenarios need region metadata, not a recompiled executable or an ISO.
    add_executable(mhfu_test_region_data tests/prepare_region_data.cpp)
    target_link_libraries(mhfu_test_region_data PRIVATE mhf_psp_recomp)
    mhf_target_warnings(mhfu_test_region_data)
    file(GLOB gathering_region_maps CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/regions/*/symbols.toml)
    foreach(disc ULES01213 ULUS10391 ULJM05500 ULJM05500-FUC unmapped)
        set(test_region ${disc})
        if(disc STREQUAL "unmapped")
            set(test_region ULES01213)
        endif()
        set(test_data ${CMAKE_CURRENT_BINARY_DIR}/test-regions/${disc})
        set(test_header ${test_data}/mhfu_region_data.hpp)
        add_custom_command(OUTPUT ${test_header}
            COMMAND mhfu_test_region_data ${CMAKE_CURRENT_SOURCE_DIR}/regions/${test_region}/symbols.toml ${test_data} ${disc}
            DEPENDS mhfu_test_region_data ${gathering_region_maps})
        set(test_name test_mhfu_gathering_runtime_${disc})
        add_executable(${test_name} tests/test_gathering_runtime.cpp host/gathering.cpp host/gathering_logic.cpp
            host/gathering_art.cpp ${test_header})
        target_include_directories(${test_name} PRIVATE host ${test_data} ${PROJECT_SOURCE_DIR}/tests/support)
        target_link_libraries(${test_name} PRIVATE mhf_psp_runtime mhf_core)
        mhf_target_warnings(${test_name})
        add_test(NAME mhfu_gathering_runtime_${disc} COMMAND ${test_name})
    endforeach()
    add_executable(test_mhfu_gathering_game_art tests/test_gathering_game_art.cpp host/gathering_game_art.cpp)
    target_include_directories(test_mhfu_gathering_game_art PRIVATE host ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_mhfu_gathering_game_art PRIVATE mhf_mhfu_data mhf_psp_tools mhf_core mhf_texture_pack mhf_texture_images)
    mhf_target_warnings(test_mhfu_gathering_game_art)
    add_test(NAME mhfu_gathering_game_art COMMAND test_mhfu_gathering_game_art)
    add_executable(test_mhfu_gathering_art tests/test_gathering_art.cpp host/gathering_art.cpp)
    target_include_directories(test_mhfu_gathering_art PRIVATE host ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_mhfu_gathering_art PRIVATE mhf_core)
    mhf_target_warnings(test_mhfu_gathering_art)
    add_test(NAME mhfu_gathering_art COMMAND test_mhfu_gathering_art)
    add_executable(test_mhfu_gathering tests/test_gathering.cpp host/gathering_logic.cpp)
    target_include_directories(test_mhfu_gathering PRIVATE host ${PROJECT_SOURCE_DIR}/tests/support)
    mhf_target_warnings(test_mhfu_gathering)
    add_test(NAME mhfu_gathering COMMAND test_mhfu_gathering)
    add_executable(test_mhfu_data_bin tests/test_data_bin.cpp)
    target_include_directories(test_mhfu_data_bin PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_mhfu_data_bin PRIVATE mhf_mhfu_data)
    mhf_target_warnings(test_mhfu_data_bin)
    add_test(NAME mhfu_data_bin COMMAND test_mhfu_data_bin)

    add_executable(test_mhfu_region_maps tests/test_region_maps.cpp)
    target_include_directories(test_mhfu_region_maps PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_compile_definitions(test_mhfu_region_maps PRIVATE MHF_MHFU_REGIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/regions")
    target_link_libraries(test_mhfu_region_maps PRIVATE mhf_psp_recomp)
    mhf_target_warnings(test_mhfu_region_maps)
    add_test(NAME mhfu_region_maps COMMAND test_mhfu_region_maps)
endif()

# The game itself, built from code recompiled on the developer's machine (never committed, CONTRIBUTING.md).
# Prepare it with games/mhfu/tools/prepare.py, then configure with -DMHF_MHFU_GENERATED=<that folder>.
set(MHF_MHFU_GENERATED "" CACHE PATH "Folder with mhf_recomp output for Freedom Unite (developer-only)")
if(MHF_MHFU_GENERATED)
    file(GLOB mhfu_generated_sources CONFIGURE_DEPENDS ${MHF_MHFU_GENERATED}/*.cpp)
    # The generated code is its own library, compiled only against the stable ABI headers, so that host-side changes
    # (frontend, SDL, options) never force the long rebuild of the game code.
    if(MSVC)
        # Debug info (/Zi) and /O2 push the compiler out of heap on the largest generated files; this directory only.
        string(REGEX REPLACE "/Zi|/O2|/Ob[0-9]" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
        string(REGEX REPLACE "/Zi|/O2|/Ob[0-9]" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    endif()
    add_library(mhfu_generated STATIC ${mhfu_generated_sources})
    target_include_directories(mhfu_generated PRIVATE ${MHF_MHFU_GENERATED})
    target_link_libraries(mhfu_generated PRIVATE mhf_psp_runtime)
    if(MSVC)
        target_compile_options(mhfu_generated PRIVATE /w /bigobj /O1)
        # A few huge generated files (several MB) make the optimizer use tens of GB, or run out of heap. Build those
        # without optimization (still fast enough: the hot code lives in the other parts).
        foreach(src ${mhfu_generated_sources})
            file(SIZE ${src} src_size)
            if(src_size GREATER 5000000)
                set_source_files_properties(${src} PROPERTIES COMPILE_OPTIONS /Od)
            endif()
        endforeach()
    else()
        target_compile_options(mhfu_generated PRIVATE -w -O1)
    endif()
    add_executable(mhfu host/main.cpp host/horn_songs.cpp host/gathering.cpp host/gathering_logic.cpp host/gathering_art.cpp
        host/gathering_game_art.cpp)
    # One program per region; the launcher picks it by the disc (launcher.cpp, game_program_name).
    set(MHF_MHFU_PROGRAM_NAME "mhfu" CACHE STRING "File name of the Freedom Unite program: mhfu (EU), mhfu-us (NA), mhfu-jp (JP) or mhfu-fuc (FUComplete)")
    set_target_properties(mhfu PROPERTIES OUTPUT_NAME ${MHF_MHFU_PROGRAM_NAME})
    # The FUComplete program leaves out what FUComplete's own options replace (host/main.cpp, kFucomplete).
    target_compile_definitions(mhfu PRIVATE MHF_MHFU_FUC=$<STREQUAL:${MHF_MHFU_PROGRAM_NAME},mhfu-fuc>)
    target_include_directories(mhfu PRIVATE ${MHF_MHFU_GENERATED})
    target_link_libraries(mhfu PRIVATE mhfu_generated mhf_psp_runtime mhf_psp_tools mhf_core mhf_mhfu_data
        mhf_texture_pack mhf_texture_images)
    mhf_psp_copy_fonts(mhfu)
    if(WIN32)
        target_sources(mhfu PRIVATE ${MHF_WINDOWS_ICON_RC})
    endif()
    if(TARGET mhf_psp_frontend)
        target_link_libraries(mhfu PRIVATE mhf_psp_frontend)
    endif()
    mhf_target_warnings(mhfu)

    if(MHF_BUILD_TESTS)
        # The song matching of host/horn_songs.cpp, which reads this release's region data (hence the generated folder).
        add_executable(test_mhfu_horn_songs tests/test_horn_songs.cpp host/horn_songs.cpp)
        target_include_directories(test_mhfu_horn_songs PRIVATE host ${MHF_MHFU_GENERATED} ${PROJECT_SOURCE_DIR}/tests/support)
        target_link_libraries(test_mhfu_horn_songs PRIVATE mhf_psp_runtime mhf_core)
        mhf_target_warnings(test_mhfu_horn_songs)
        add_test(NAME mhfu_horn_songs COMMAND test_mhfu_horn_songs)
    endif()
endif()
