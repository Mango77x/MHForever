# ATRAC3/ATRAC3+ decoders extracted from FFmpeg (LGPL-2.1-or-later), via PPSSPP's ext/at3_standalone.
# See THIRD_PARTY.md. Warnings are off: this is upstream code we keep unchanged.
add_library(at3_standalone STATIC
    atrac.cpp
    atrac3.cpp
    atrac3plus.cpp
    atrac3plusdec.cpp
    atrac3plusdsp.cpp
    compat.cpp
    fft.cpp
    get_bits.cpp
    mem.cpp
)
target_include_directories(at3_standalone PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(at3_standalone PRIVATE -w)
set_target_properties(at3_standalone PROPERTIES POSITION_INDEPENDENT_CODE ON)
