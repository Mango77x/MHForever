#pragma once

// Developer aid: saves the displayed PSP framebuffer as PNG files. Output stays on the developer's machine; game
// screenshots are never committed (see CONTRIBUTING.md).

#include "mhf/psp/runtime.hpp"

#include <cstdint>
#include <filesystem>

namespace mhf::psp {

// Writes the framebuffer currently shown by sceDisplay (480x272) to `path`. Returns false when no framebuffer is set.
bool dump_display(Runtime &rt, const std::filesystem::path &path);

// Installs a host hook that dumps one frame every `every` vblanks into `folder` (frame_000001.png, ...).
void enable_frame_dumps(Runtime &rt, const std::filesystem::path &folder, unsigned every);

} // namespace mhf::psp
