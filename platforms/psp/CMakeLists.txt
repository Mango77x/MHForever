# PSP platform: recompiler tools, HLE runtime, GE renderer and audio (docs/02-architecture.md §1).
# The runtime arrives in phase 1 (docs/10-roadmap.md).

add_library(mhf_psp_tools STATIC
    tools/iso9660.cpp
)
target_include_directories(mhf_psp_tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
mhf_target_warnings(mhf_psp_tools)

add_library(mhf_psp INTERFACE)
target_link_libraries(mhf_psp INTERFACE psprecomp_core mhf_core mhf_psp_tools)

if(MHF_BUILD_TESTS)
    add_executable(test_iso9660 tests/test_iso9660.cpp)
    target_include_directories(test_iso9660 PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(test_iso9660 PRIVATE mhf_psp_tools)
    mhf_target_warnings(test_iso9660)
    add_test(NAME psp_iso9660 COMMAND test_iso9660)
endif()
