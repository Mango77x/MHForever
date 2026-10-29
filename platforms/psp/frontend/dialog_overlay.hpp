#pragma once

// Draws the system dialogs (mhf/psp/dialog.hpp) as an image laid over the game: a dimmed backdrop and a rounded
// glass panel in the launcher's style (dark surfaces, amber accent). CPU rasterized with stb_truetype; it only runs
// when the dialog changes.

#include "mhf/psp/dialog.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace mhf::psp {

class DialogOverlay {
public:
    // `font_directory` holds LiberationSans-Regular.ttf (RuntimeOptions::font_directory).
    explicit DialogOverlay(const std::filesystem::path &font_directory);
    ~DialogOverlay();

    // Renders `view` at `scale` times the 480x272 screen into premultiplied RGBA (R in the low byte). An empty
    // image (all zero) when the view is Kind::None. Returns false when no font could be loaded.
    bool render(const DialogView &view, int scale, std::vector<std::uint32_t> &pixels);
    // The same at `width` x `height` pixels (scaled by height / 272): the size it is shown at, so its text stays sharp.
    bool render(const DialogView &view, int width, int height, std::vector<std::uint32_t> &pixels);

    [[nodiscard]] static int width(int scale) { return 480 * scale; }
    [[nodiscard]] static int height(int scale) { return 272 * scale; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::psp
