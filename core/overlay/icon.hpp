#pragma once

// The project's icon, for the overlays (generated from assets/branding by scripts/gen_overlay_icon.py).

#include <cstdint>

namespace mhf::overlay {

inline constexpr int kIconSize = 64;
// kIconSize x kIconSize pixels, premultiplied RGBA (r | g << 8 | b << 16 | a << 24), from the top row.
extern const std::uint32_t kIcon[kIconSize * kIconSize];

} // namespace mhf::overlay
