# pokke's PSP save-data cryptography (see NOTICE.md).
add_library(mhf_save_crypto STATIC save_crypto/aes.cpp save_crypto/save_crypto.cpp)
target_include_directories(mhf_save_crypto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mhf_save_crypto PUBLIC cxx_std_20)
