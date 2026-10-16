# PSP platform: recompiler tools, HLE runtime, GE renderer and audio (docs/02-architecture.md §1).
# Phase 0: only links the vendored PSPRecomp framework. The runtime arrives in phase 1 (docs/10-roadmap.md).
add_library(mhf_psp INTERFACE)
target_link_libraries(mhf_psp INTERFACE psprecomp_core mhf_core)
