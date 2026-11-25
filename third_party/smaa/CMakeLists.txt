# Reference SMAA 1x shader and precomputed LUTs (https://github.com/iryoku/smaa, MIT).
set(_smaa_hlsl_cpp ${CMAKE_CURRENT_BINARY_DIR}/smaa_hlsl.cpp)
add_custom_command(
    OUTPUT ${_smaa_hlsl_cpp}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/SMAA.hlsl
        -DOUTPUT=${_smaa_hlsl_cpp}
        -DSYMBOL=kSmaaHlsl
        "-DCOMMENT=third_party/smaa/SMAA.hlsl (iryoku/smaa, MIT)"
        -P ${PROJECT_SOURCE_DIR}/launcher/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/SMAA.hlsl ${PROJECT_SOURCE_DIR}/launcher/embed.cmake
    COMMENT "Embedding SMAA.hlsl"
    VERBATIM)

add_library(mhf_smaa STATIC ${_smaa_hlsl_cpp})
target_include_directories(mhf_smaa PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
