# PSP runtime: kernel, scheduler and HLE modules for recompiled programs (docs/05-runtime-hle.md).

add_library(mhf_psp_runtime STATIC
    src/fiber.cpp
    src/frame_dump.cpp
    src/hle_display.cpp
    src/hle_ge.cpp
    src/hle_io.cpp
    src/hle_media.cpp
    src/hle_atrac.cpp
    src/hle_utility.cpp
    src/hle_font.cpp
    src/hle_sas.cpp
    src/hle_savedata.cpp
    src/hle_misc.cpp
    src/hle_sysmem.cpp
    src/hle_system.cpp
    src/hle_thread.cpp
    src/interpreter.cpp
    src/interpreter_vfpu.cpp
    src/kernel.cpp
    src/profiler.cpp
    src/runtime.cpp
    src/sysmem.cpp
    ../gpu/ge.cpp
)
target_include_directories(mhf_psp_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gpu)
target_link_libraries(mhf_psp_runtime PUBLIC psprecomp_core mhf_psp_tools PRIVATE mhf_stb_truetype at3_standalone mhf_save_crypto)
mhf_target_warnings(mhf_psp_runtime)
if(WIN32)
    target_link_libraries(mhf_psp_runtime PRIVATE winmm dbghelp) # timeBeginPeriod; the sampling profiler (profiler.cpp)
endif()

# mhf_psp_copy_fonts(<target>): puts the free fonts used by sceLibFont next to the executable, in fonts/.
function(mhf_psp_copy_fonts target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${PROJECT_SOURCE_DIR}/third_party/fonts/liberation-sans/LiberationSans-Regular.ttf
                ${PROJECT_SOURCE_DIR}/third_party/fonts/liberation-sans/LICENSE
                $<TARGET_FILE_DIR:${target}>/fonts/
        VERBATIM)
endfunction()

# mhf_add_recompiled_program(<name> ELF <path> [PARTS <n>] [MAIN <source>])
# Recompiles <path> at build time with mhf_recomp and links the output with the runtime into executable <name>.
# The generated sources live in the build tree only (never committed, CONTRIBUTING.md).
function(mhf_add_recompiled_program name)
    cmake_parse_arguments(ARG "" "ELF;PARTS;MAIN;DEPENDS" "" ${ARGN})
    if(NOT ARG_PARTS)
        set(ARG_PARTS 4)
    endif()
    if(NOT ARG_MAIN)
        set(ARG_MAIN ${PROJECT_SOURCE_DIR}/platforms/psp/runtime/host/homebrew_main.cpp)
    endif()
    set(out ${CMAKE_CURRENT_BINARY_DIR}/generated/${name})
    set(sources ${out}/${name}.hpp ${out}/${name}_table.cpp)
    math(EXPR last "${ARG_PARTS} - 1")
    foreach(i RANGE ${last})
        list(APPEND sources ${out}/${name}_part${i}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${sources}
        COMMAND mhf_recomp --elf ${ARG_ELF} --out ${out} --name ${name} --parts ${ARG_PARTS}
        DEPENDS mhf_recomp ${ARG_ELF} ${ARG_DEPENDS}
        COMMENT "Recompiling ${name}"
        VERBATIM)
    add_executable(${name} ${ARG_MAIN} ${sources})
    target_include_directories(${name} PRIVATE ${out})
    target_compile_definitions(${name} PRIVATE MHF_PROGRAM=${name})
    target_link_libraries(${name} PRIVATE mhf_psp_runtime)
    if(TARGET mhf_psp_frontend)
        target_link_libraries(${name} PRIVATE mhf_psp_frontend)
    endif()
    mhf_psp_copy_fonts(${name})
    # Generated code: no warnings, keep optimization even in debug builds so large functions stay manageable.
    if(MSVC)
        set_source_files_properties(${sources} PROPERTIES COMPILE_OPTIONS "/w;/bigobj")
    else()
        set_source_files_properties(${sources} PROPERTIES COMPILE_OPTIONS "-w;-O1")
    endif()
endfunction()
