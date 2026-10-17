# PSP platform: recompiler tools, HLE runtime, GE renderer and audio (docs/02-architecture.md §1).
# The runtime arrives in phase 1 (docs/10-roadmap.md).

add_library(mhf_psp_tools STATIC
    tools/iso9660.cpp
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

if(MHF_WITH_SDL)
    add_library(mhf_psp_frontend STATIC frontend/psp_frontend.cpp frontend/gl_backend.cpp)
    target_include_directories(mhf_psp_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/frontend
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gpu)
    target_link_libraries(mhf_psp_frontend PUBLIC mhf_psp_runtime PRIVATE mhf_core_host)
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
endif()
