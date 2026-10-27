# Kethen's aemu_postoffice client: the adhocctl matchmaking client and the postoffice relay client that PSP ad hoc
# servers speak (PPSSPP and aemu use the same code). See NOTICE.md. The PSP-only files of the original are not included.
add_library(mhf_aemu_postoffice STATIC
    client/postoffice.c
    client/enet.c
    client/list.c
    client/log_impl_stdc.c
    client/postoffice_mem_stdc.c
    client/mutex_impl_cpp.cpp
    client/delay_impl_cpp.cpp
    adhocctl/client/adhocctl.c
    adhocctl/client/adhocctl_mem.c
)
if(WIN32)
    target_sources(mhf_aemu_postoffice PRIVATE client/sock_impl_windows.c)
    target_link_libraries(mhf_aemu_postoffice PUBLIC ws2_32 winmm)
    target_compile_definitions(mhf_aemu_postoffice PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    target_sources(mhf_aemu_postoffice PRIVATE client/sock_impl_linux.c)
    find_package(Threads REQUIRED)
    target_link_libraries(mhf_aemu_postoffice PUBLIC Threads::Threads)
endif()
# Users include "postoffice_client.h" and "adhocctl.h".
target_include_directories(mhf_aemu_postoffice PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/client
    ${CMAKE_CURRENT_SOURCE_DIR}/adhocctl/client
)
# Warnings are off: this is upstream code we keep unchanged.
target_compile_options(mhf_aemu_postoffice PRIVATE -w)
