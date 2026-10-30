# xxHash v0.8.3 (https://github.com/Cyan4973/xxHash), BSD-2-Clause (LICENSE). Unmodified xxhash.h, used header-only.
add_library(mhf_xxhash INTERFACE)
target_include_directories(mhf_xxhash SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
