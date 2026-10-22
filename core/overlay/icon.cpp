#include "overlay/icon.hpp"

namespace mhf::overlay {

const std::uint32_t kIcon[kIconSize * kIconSize] = {
#include "overlay/icon.inc"
};

} // namespace mhf::overlay
