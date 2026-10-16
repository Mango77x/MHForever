# Our own PSP test programs (docs/10-roadmap.md phase 1). They are built with the pspdev toolchain when it is
# available (PSPDEV environment variable or /opt/pspdev), recompiled with mhf_recomp and run natively as tests.

set(MHF_PSPDEV "$ENV{PSPDEV}" CACHE PATH "pspdev toolchain root")
if(NOT MHF_PSPDEV AND EXISTS /opt/pspdev/bin/psp-gcc)
    set(MHF_PSPDEV /opt/pspdev CACHE PATH "pspdev toolchain root" FORCE)
endif()
if(NOT MHF_PSPDEV OR NOT EXISTS ${MHF_PSPDEV}/bin/psp-gcc${CMAKE_EXECUTABLE_SUFFIX})
    message(STATUS "pspdev not found: homebrew tests disabled")
    return()
endif()
find_program(MHF_MAKE NAMES make gmake REQUIRED)

# mhf_add_homebrew(<name> EXPECT <regex>)
function(mhf_add_homebrew name)
    cmake_parse_arguments(ARG "" "EXPECT" "" ${ARGN})
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${name})
    set(work ${CMAKE_CURRENT_BINARY_DIR}/${name}_psp)
    file(GLOB inputs CONFIGURE_DEPENDS ${src}/*)
    add_custom_command(
        OUTPUT ${work}/${name}.elf
        COMMAND ${CMAKE_COMMAND} -E make_directory ${work}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${src} ${work}
        COMMAND ${CMAKE_COMMAND} -E env PATH=${MHF_PSPDEV}/bin:$ENV{PATH} PSPDEV=${MHF_PSPDEV}
                PSPSDK=${MHF_PSPDEV}/psp/sdk ${MHF_MAKE} -C ${work} --no-print-directory
        DEPENDS ${inputs}
        COMMENT "Building homebrew ${name} with pspdev"
        VERBATIM)
    add_custom_target(${name}_elf DEPENDS ${work}/${name}.elf)
    mhf_add_recompiled_program(hb_${name} ELF ${work}/${name}.elf PARTS 2)
    add_dependencies(hb_${name} ${name}_elf)
    add_test(NAME homebrew_${name} COMMAND hb_${name} ${work}/${name}.elf --memstick ${work}/memstick)
    set_tests_properties(homebrew_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${ARG_EXPECT}" TIMEOUT 60)
endfunction()

mhf_add_homebrew(hello EXPECT "hello from the PSP runtime\ncount 1\ncount 2\ncount 3")
mhf_add_homebrew(cputest EXPECT "cputest: [0-9]+ passed, 0 failed")
mhf_add_homebrew(threadtest EXPECT "threadtest: [0-9]+ passed, 0 failed")
