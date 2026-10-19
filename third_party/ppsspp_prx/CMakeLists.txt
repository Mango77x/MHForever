# PSP PRX decryption taken from PPSSPP (see NOTICE.md). Used only by the launcher's executable preparation.
cmake_minimum_required(VERSION 3.20)
include_guard(GLOBAL)
enable_language(C)

add_library(mhf_prx STATIC
    Core/ELF/PrxDecrypter.cpp
    ext/libkirk/AES.c
    ext/libkirk/SHA1.c
    ext/libkirk/bn.c
    ext/libkirk/ec.c
    ext/libkirk/kirk_engine.c
)
# `shim` stands in for the few PPSSPP Common/ headers; the PPSSPP sources include "ext/libkirk/..." and "Core/ELF/...".
target_include_directories(mhf_prx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/shim)
target_include_directories(mhf_prx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ext/libkirk)
if(MSVC)
    target_compile_options(mhf_prx PRIVATE /w)
else()
    target_compile_options(mhf_prx PRIVATE -w)
endif()
