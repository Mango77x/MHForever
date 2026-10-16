# stb_truetype v1.26 (https://github.com/nothings/stb), MIT or public domain. Unmodified.
add_library(mhf_stb_truetype STATIC stb_truetype.cpp)
target_include_directories(mhf_stb_truetype SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mhf_stb_truetype PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)
