# Noto Sans JP Regular (SIL OFL 1.1), the second font of sceLibFont: the Japanese text Liberation Sans has no glyphs for
# (platforms/psp/runtime/src/hle_font.cpp). At 4.5 MB it is too large to keep in the repository, so it is downloaded once
# and checked against its SHA-256; its licence is stored in third_party/fonts/noto-sans-jp/. An offline build can pass the
# file with -DMHF_NOTO_SANS_JP=<path>.
set(MHF_NOTO_SANS_JP_URL
    "https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf")
set(MHF_NOTO_SANS_JP_SHA256 "dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073")
set(MHF_NOTO_SANS_JP "${CMAKE_BINARY_DIR}/_deps/fonts/NotoSansJP-Regular.otf" CACHE FILEPATH
    "Noto Sans JP Regular (noto-cjk Sans2.004), downloaded when missing")

include(${CMAKE_CURRENT_LIST_DIR}/download.cmake)
mhf_download("${MHF_NOTO_SANS_JP_URL}" "${MHF_NOTO_SANS_JP}" "${MHF_NOTO_SANS_JP_SHA256}" _noto_problem)
if(_noto_problem)
    message(FATAL_ERROR "Could not get Noto Sans JP: ${_noto_problem}. "
                        "Download it from ${MHF_NOTO_SANS_JP_URL} and pass -DMHF_NOTO_SANS_JP=<path>.")
endif()
