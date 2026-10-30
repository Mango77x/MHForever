# Basis Universal transcoder (KTX2 reader, UASTC/ETC1S to RGBA) plus the zstd it needs for KTX2 supercompression.
# Sources: https://github.com/BinomialLLC/basis_universal at commit 073eb6a0a34a23d77e75e54f41dcd9653a52da83 (transcoder/ and zstd/), unmodified.
# Licenses: Apache-2.0 (LICENSE) and zstd's BSD-3-Clause option (zstd/LICENSE). See README.md.
enable_language(C)
add_library(mhf_basisu STATIC transcoder/basisu_transcoder.cpp zstd/zstd.c)
target_include_directories(mhf_basisu SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/transcoder)
target_compile_definitions(mhf_basisu PUBLIC BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1)
target_compile_options(mhf_basisu PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)
