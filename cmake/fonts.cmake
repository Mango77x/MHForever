# Noto Sans JP Regular (SIL OFL 1.1), the second font of sceLibFont: the Japanese text Liberation Sans has no glyphs for
# (platforms/psp/runtime/src/hle_font.cpp). At 4.5 MB it is too large to keep in the repository, so it is downloaded once
# and checked against its SHA-256; its licence is stored in third_party/fonts/noto-sans-jp/. An offline build can pass the
# file with -DMHF_NOTO_SANS_JP=<path>.
set(MHF_NOTO_SANS_JP_URL
    "https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf")
set(MHF_NOTO_SANS_JP_SHA256 "dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073")
set(MHF_NOTO_SANS_JP "${CMAKE_BINARY_DIR}/_deps/fonts/NotoSansJP-Regular.otf" CACHE FILEPATH
    "Noto Sans JP Regular (noto-cjk Sans2.004), downloaded when missing")

set(_noto_ok OFF)
if(EXISTS "${MHF_NOTO_SANS_JP}")
    file(SHA256 "${MHF_NOTO_SANS_JP}" _noto_hash)
    if(_noto_hash STREQUAL MHF_NOTO_SANS_JP_SHA256)
        set(_noto_ok ON)
    endif()
endif()
if(NOT _noto_ok)
    message(STATUS "Downloading Noto Sans JP (${MHF_NOTO_SANS_JP_URL})")
    file(DOWNLOAD "${MHF_NOTO_SANS_JP_URL}" "${MHF_NOTO_SANS_JP}"
         EXPECTED_HASH SHA256=${MHF_NOTO_SANS_JP_SHA256} TLS_VERIFY ON STATUS _noto_status)
    list(GET _noto_status 0 _noto_code)
    if(NOT _noto_code EQUAL 0)
        list(GET _noto_status 1 _noto_message)
        message(FATAL_ERROR "Could not download Noto Sans JP: ${_noto_message}. "
                            "Download it from ${MHF_NOTO_SANS_JP_URL} and pass -DMHF_NOTO_SANS_JP=<path>.")
    endif()
endif()
