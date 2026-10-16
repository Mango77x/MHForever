#pragma once

#include <string_view>

namespace mhf {

// Version of the platform (launcher + runtime), independent of any game module.
[[nodiscard]] std::string_view version() noexcept;

} // namespace mhf
