#pragma once

// MH-Forever: replaces PPSSPP's ppsspp_config.h for atrac3plusdsp.cpp (architecture detection only).

#define PPSSPP_ARCH(x) (MHF_AT3_ARCH_##x)

#if defined(__x86_64__) || defined(_M_X64)
#define MHF_AT3_ARCH_AMD64 1
#define MHF_AT3_ARCH_SSE2 1
#else
#define MHF_AT3_ARCH_AMD64 0
#endif

#if defined(__i386__) || defined(_M_IX86)
#define MHF_AT3_ARCH_X86 1
#define MHF_AT3_ARCH_SSE2 1
#else
#define MHF_AT3_ARCH_X86 0
#endif

#ifndef MHF_AT3_ARCH_SSE2
#define MHF_AT3_ARCH_SSE2 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define MHF_AT3_ARCH_ARM64 1
#define MHF_AT3_ARCH_ARM_NEON 1
#else
#define MHF_AT3_ARCH_ARM64 0
#if defined(__ARM_NEON)
#define MHF_AT3_ARCH_ARM_NEON 1
#else
#define MHF_AT3_ARCH_ARM_NEON 0
#endif
#endif
