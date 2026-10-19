# webview (MIT): the system web view (WebView2 on Windows, WebKitGTK on Linux) that shows the launcher UI.
# Header-only; fetched at configure time, pinned to the 0.12.0 release commit.
include(FetchContent)
FetchContent_Declare(webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG 3ab4b5d722438fc8a13e6ca830c5e2372d19a01d # 0.12.0
    GIT_SHALLOW FALSE)
FetchContent_GetProperties(webview)
if(NOT webview_POPULATED)
    FetchContent_Populate(webview)
endif()
add_library(mhf_webview INTERFACE)
target_include_directories(mhf_webview SYSTEM INTERFACE ${webview_SOURCE_DIR}/core/include)

if(WIN32)
    # WebView2 SDK headers (Microsoft, BSD-3-Clause), from the official NuGet package unless a folder holding
    # WebView2.h is given (useful offline or when cross-compiling).
    set(MHF_WEBVIEW2_INCLUDE_DIR "" CACHE PATH "Folder containing WebView2.h (empty: download the NuGet package)")
    if(MHF_WEBVIEW2_INCLUDE_DIR STREQUAL "")
        FetchContent_Declare(webview2_sdk
            URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.1150.38
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_GetProperties(webview2_sdk)
        if(NOT webview2_sdk_POPULATED)
            FetchContent_Populate(webview2_sdk)
        endif()
        set(MHF_WEBVIEW2_INCLUDE_DIR ${webview2_sdk_SOURCE_DIR}/build/native/include)
    endif()
    target_include_directories(mhf_webview SYSTEM INTERFACE ${MHF_WEBVIEW2_INCLUDE_DIR})
    if(NOT MSVC)
        # MinGW ships eventtoken.h in lower case; WebView2.h asks for EventToken.h. Not for MSVC/clang-cl: Windows
        # filesystems ignore case, so the shim would include itself and hide the SDK's real header (winrt/eventtoken.h,
        # found through the Developer shell's INCLUDE), leaving EventRegistrationToken undefined.
        set(_shim ${CMAKE_BINARY_DIR}/webview2-shim)
        file(WRITE ${_shim}/EventToken.h "#pragma once\n#include <eventtoken.h>\n")
        target_include_directories(mhf_webview SYSTEM INTERFACE ${_shim})
    endif()
    target_link_libraries(mhf_webview INTERFACE advapi32 ole32 shell32 shlwapi user32 version)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MHF_WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1 gtk+-3.0)
    target_link_libraries(mhf_webview INTERFACE PkgConfig::MHF_WEBKIT dl)
endif()
