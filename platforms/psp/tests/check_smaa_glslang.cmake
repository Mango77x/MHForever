# Dumps the assembled SMAA GLSL 330 stages with test_smaa and compiles+links all three pairs with glslangValidator -l.
foreach(var IN ITEMS TEST_SMAA GLSLANG WORK_DIR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "missing required variable: ${var}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(
    COMMAND "${TEST_SMAA}" --dump-shaders "${WORK_DIR}"
    RESULT_VARIABLE dump_rc
)
if(NOT dump_rc EQUAL 0)
    message(FATAL_ERROR "test_smaa --dump-shaders failed with exit code ${dump_rc}")
endif()

foreach(pass IN ITEMS edge weight blend)
    execute_process(
        COMMAND "${GLSLANG}" -l "${WORK_DIR}/${pass}.vert" "${WORK_DIR}/${pass}.frag"
        RESULT_VARIABLE glsl_rc
    )
    if(NOT glsl_rc EQUAL 0)
        message(FATAL_ERROR "glslangValidator -l failed for SMAA pass '${pass}' (exit code ${glsl_rc})")
    endif()
endforeach()
