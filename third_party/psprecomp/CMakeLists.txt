# Vendored PSPRecomp framework (MIT). See README.md in this folder for origin and local changes.
# Only the reusable framework is vendored; upstream's game profiles are intentionally excluded.

add_library(psprecomp_core STATIC
    src/decoder.cpp
    src/deflate.cpp
    src/elf32.cpp
    src/guest_memory.cpp
    src/nid_registry.cpp
    src/program_analysis.cpp
    src/runtime.cpp
    src/sha256.cpp
)
target_include_directories(psprecomp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(psprecomp_core PUBLIC cxx_std_20)
target_compile_definitions(psprecomp_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

add_executable(psp_analyze tools/analyzer_main.cpp)
target_link_libraries(psp_analyze PRIVATE psprecomp_core)

add_executable(psp_recomp tools/codegen_main.cpp)
target_link_libraries(psp_recomp PRIVATE psprecomp_core)

add_executable(psp_dump_function tools/dump_function.cpp)
target_link_libraries(psp_dump_function PRIVATE psprecomp_core)

if(MHF_BUILD_TESTS)
    add_executable(psprecomp_tests tests/test_main.cpp)
    target_link_libraries(psprecomp_tests PRIVATE psprecomp_core)
    add_dependencies(psprecomp_tests psp_recomp)
    target_compile_definitions(psprecomp_tests PRIVATE PSPRECOMP_CODEGEN_PATH="$<TARGET_FILE:psp_recomp>")
    add_test(NAME psprecomp_tests COMMAND psprecomp_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
