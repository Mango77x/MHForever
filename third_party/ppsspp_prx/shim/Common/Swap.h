// MH-Forever shim (GPL-3.0-or-later): PPSSPP's _le types are plain integers on little-endian hosts.
// The decryptor reads PSP data in place, so a big-endian host is refused at compile time.
#pragma once
#include "Common/CommonTypes.h"
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "ppsspp_prx shim supports little-endian hosts only"
#endif
typedef u16 u16_le; typedef u32 u32_le; typedef u64 u64_le;
typedef s16 s16_le; typedef s32 s32_le; typedef s64 s64_le;
