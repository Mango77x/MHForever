# PSP platform: recompiler tools, HLE runtime, GE renderer and audio (docs/02-architecture.md §1).
# The runtime arrives in phase 1 (docs/10-roadmap.md).

add_library(mhf_psp_tools STATIC
    tools/iso9660.cpp
    tools/param_sfo.cpp
)
target_include_directories(mhf_psp_tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
mhf_target_warnings(mhf_psp_tools)

# Function-level recompiler (docs/04-recompiler.md).
add_library(mhf_psp_recomp STATIC
    recomp/analysis.cpp
    recomp/emitter.cpp
    recomp/instruction_emitter.cpp
)
target_include_directories(mhf_psp_recomp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/recomp)
target_link_libraries(mhf_psp_recomp PUBLIC psprecomp_core)
mhf_target_warnings(mhf_psp_recomp)

add_executable(mhf_recomp recomp/main.cpp)
target_link_libraries(mhf_recomp PRIVATE mhf_psp_recomp)
mhf_target_warnings(mhf_recomp)

add_subdirectory(runtime)

# Frame interpolation's matching (frontend/frame_matching.*): pure, so the tests use it without SDL.
add_library(mhf_psp_interpolation STATIC frontend/frame_matching.cpp)
target_include_directories(mhf_psp_interpolation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/frontend ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
mhf_target_warnings(mhf_psp_interpolation)

# Texture packs in PPSSPP's format (frontend/texture_pack.*: the ini, keys and lookups; frontend/texture_images.*: the
# images and their loader). The launcher reviews packs with the first; neither needs SDL.
add_library(mhf_texture_pack STATIC frontend/texture_pack.cpp)
target_include_directories(mhf_texture_pack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
target_link_libraries(mhf_texture_pack PRIVATE mhf_xxhash)
mhf_target_warnings(mhf_texture_pack)
add_library(mhf_texture_images STATIC frontend/texture_images.cpp)
target_include_directories(mhf_texture_images PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
target_link_libraries(mhf_texture_images PRIVATE mhf_basisu mhf_stb_image)
mhf_target_warnings(mhf_texture_images)

# SMAA 1x shader assembly and LUT views (frontend/smaa_shaders.*): pure C++, no SDL/OpenGL dependency.
add_library(mhf_smaa_shaders STATIC frontend/smaa_shaders.cpp)
target_include_directories(mhf_smaa_shaders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
target_link_libraries(mhf_smaa_shaders PRIVATE mhf_smaa)
mhf_target_warnings(mhf_smaa_shaders)

if(MHF_WITH_SDL)
    add_library(mhf_psp_frontend STATIC frontend/psp_frontend.cpp frontend/gl_backend.cpp frontend/dialog_overlay.cpp)
    target_include_directories(mhf_psp_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/frontend
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    # mhf_overlay is public: psp_frontend.hpp takes the horn's song panel (overlay/song_panel.hpp).
    target_link_libraries(mhf_psp_frontend PUBLIC mhf_psp_runtime mhf_overlay PRIVATE mhf_core_host mhf_stb_truetype mhf_psp_interpolation
                                                                    mhf_texture_pack mhf_texture_images mhf_smaa_shaders)
    target_compile_definitions(mhf_psp_frontend PUBLIC MHF_HAS_FRONTEND=1)
    mhf_target_warnings(mhf_psp_frontend)
endif()

add_library(mhf_psp INTERFACE)
target_link_libraries(mhf_psp INTERFACE psprecomp_core mhf_core mhf_psp_tools mhf_psp_runtime)

if(MHF_BUILD_TESTS)
    add_executable(test_iso9660 tests/test_iso9660.cpp)
    target_include_directories(test_iso9660 PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_iso9660 PRIVATE mhf_psp_tools)
    mhf_target_warnings(test_iso9660)
    add_test(NAME psp_iso9660 COMMAND test_iso9660)

    add_executable(test_hle_io tests/test_hle_io.cpp)
    target_include_directories(test_hle_io PRIVATE ${PROJECT_SOURCE_DIR}/tests/support ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src)
    target_link_libraries(test_hle_io PRIVATE mhf_psp_runtime)
    mhf_target_warnings(test_hle_io)
    add_test(NAME psp_hle_io COMMAND test_hle_io)

    add_executable(test_frame_matching tests/test_frame_matching.cpp)
    target_include_directories(test_frame_matching PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_frame_matching PRIVATE mhf_psp_interpolation)
    mhf_target_warnings(test_frame_matching)
    add_test(NAME psp_frame_matching COMMAND test_frame_matching)

    add_executable(test_texture_pack tests/test_texture_pack.cpp)
    target_include_directories(test_texture_pack PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_texture_pack PRIVATE mhf_texture_pack mhf_texture_images mhf_xxhash)
    mhf_target_warnings(test_texture_pack)
    add_test(NAME psp_texture_pack COMMAND test_texture_pack)

    add_executable(test_texture_filter tests/test_texture_filter.cpp)
    target_include_directories(test_texture_filter PRIVATE ${PROJECT_SOURCE_DIR}/tests/support ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
    mhf_target_warnings(test_texture_filter)
    add_test(NAME psp_texture_filter COMMAND test_texture_filter)

    add_executable(test_smaa tests/test_smaa.cpp)
    target_include_directories(test_smaa PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_smaa PRIVATE mhf_smaa_shaders)
    mhf_target_warnings(test_smaa)
    add_test(NAME psp_smaa COMMAND test_smaa)

    if(MHF_WITH_SDL)
        add_executable(test_smaa_gl tests/test_smaa_gl.cpp)
        target_include_directories(test_smaa_gl PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
        target_link_libraries(test_smaa_gl PRIVATE mhf_smaa_shaders SDL3::SDL3-static)
        mhf_target_warnings(test_smaa_gl)
        add_test(NAME psp_smaa_gl COMMAND test_smaa_gl)
        set_tests_properties(psp_smaa_gl PROPERTIES SKIP_RETURN_CODE 77 LABELS opengl)
        if(NOT WIN32)
            set_tests_properties(psp_smaa_gl PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
        endif()
    endif()

    find_program(MHF_GLSLANG_VALIDATOR NAMES glslangValidator)
    if(MHF_GLSLANG_VALIDATOR)
        add_test(NAME psp_smaa_glslang
                 COMMAND ${CMAKE_COMMAND}
                     -DTEST_SMAA=$<TARGET_FILE:test_smaa>
                     -DGLSLANG=${MHF_GLSLANG_VALIDATOR}
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/smaa_glslang_check
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_smaa_glslang.cmake)
    endif()

    add_executable(test_adhocctl tests/test_adhocctl.cpp)
    target_include_directories(test_adhocctl PRIVATE ${PROJECT_SOURCE_DIR}/tests/support ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src)
    target_link_libraries(test_adhocctl PRIVATE mhf_psp_runtime)
    mhf_target_warnings(test_adhocctl)
    add_test(NAME psp_adhocctl COMMAND test_adhocctl)

    # Against a real ad hoc server; skipped unless MHF_ADHOC_TEST_SERVER names one.
    add_executable(test_adhocctl_server tests/test_adhocctl_server.cpp)
    target_include_directories(test_adhocctl_server PRIVATE ${PROJECT_SOURCE_DIR}/tests/support ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src)
    target_link_libraries(test_adhocctl_server PRIVATE mhf_psp_runtime)
    mhf_target_warnings(test_adhocctl_server)
    add_test(NAME psp_adhocctl_server COMMAND test_adhocctl_server)

    add_executable(test_save_crypto tests/test_save_crypto.cpp)
    target_include_directories(test_save_crypto PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_save_crypto PRIVATE mhf_save_crypto)
    mhf_target_warnings(test_save_crypto)
    add_test(NAME psp_save_crypto COMMAND test_save_crypto)

    add_executable(test_hle_savedata tests/test_hle_savedata.cpp)
    target_include_directories(test_hle_savedata PRIVATE ${PROJECT_SOURCE_DIR}/tests/support ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src)
    target_link_libraries(test_hle_savedata PRIVATE mhf_psp_runtime mhf_save_crypto)
    mhf_target_warnings(test_hle_savedata)
    add_test(NAME psp_hle_savedata COMMAND test_hle_savedata)

    add_executable(test_audio_mix tests/test_audio_mix.cpp)
    target_include_directories(test_audio_mix PRIVATE ${PROJECT_SOURCE_DIR}/tests/support ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src)
    target_link_libraries(test_audio_mix PRIVATE mhf_psp_runtime)
    mhf_target_warnings(test_audio_mix)
    add_test(NAME psp_audio_mix COMMAND test_audio_mix)

    add_executable(test_region_data tests/test_region_data.cpp)
    target_include_directories(test_region_data PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_region_data PRIVATE mhf_psp_recomp)
    mhf_target_warnings(test_region_data)
    add_test(NAME psp_region_data COMMAND test_region_data)
endif()
