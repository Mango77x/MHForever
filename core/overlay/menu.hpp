#pragma once

// The in-game menu (docs/07-launcher-ux.md §3c): tabs across the top, a list of rows in the middle that the player moves
// through with the keyboard or the controller while the game is paused. This file is the model and the navigation;
// MenuRenderer draws it. Console- and game-independent: the program that owns the menu builds the tabs and rows,
// reacts to what changed and rebuilds them.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mhf::overlay {

struct MenuItem {
    enum class Kind {
        Button,  // Accept activates it
        Slider,  // Left/Right change `value` by `step` within min..max
        Toggle,  // `value` 0 or 1; Left/Right/Accept flip it
        Choice,  // `value` is an index into `options`; Left/Right/Accept move it (wrapping with Accept)
        Info,    // text only, never selected
        Heading, // a title for the rows below, never selected
    };
    Kind kind = Kind::Button;
    int id = 0;          // what the owner calls it
    std::string label;
    std::string detail;  // dim second line (optional)
    int value = 0;
    int min = 0, max = 100, step = 5;
    std::vector<std::string> options;
    bool danger = false; // a button that ends or deletes something
    bool enabled = true; // disabled rows are shown dim and skipped
};

struct MenuTab {
    std::string name;
    std::vector<MenuItem> items;
};

struct MenuView {
    std::string title;    // shown small at the top left (the game's name)
    std::vector<MenuTab> tabs;
    int tab = 0;          // the tab shown
    int selected = 0;     // row of that tab
    bool tabs_focused = false; // the focus is on the tab bar (Left/Right change tab)
    std::string hint;     // input hint line at the bottom
    std::string corner;   // small text at the top right ("Esc  Resume")
    std::uint64_t version = 0; // changes whenever anything above changes

    [[nodiscard]] const std::vector<MenuItem> &items() const {
        static const std::vector<MenuItem> none;
        return tab >= 0 && tab < static_cast<int>(tabs.size()) ? tabs[static_cast<std::size_t>(tab)].items : none;
    }
};

class Menu {
public:
    enum class Key { Up, Down, Left, Right, Accept, Back, PrevTab, NextTab };

    struct Event {
        enum class Kind { None, Activated, Changed, Back } kind = Kind::None;
        int id = 0;    // the row concerned (Activated, Changed)
        int value = 0; // Changed: the new value
    };

    // Shows `view`. The tab stays the one with the same name and the selection on the row with the same id when there
    // are such; otherwise the first tab and its first row that can be selected.
    void set(MenuView view);
    [[nodiscard]] const MenuView &view() const { return view_; }

    // Applies a key; returns what the owner has to know about.
    Event press(Key key);

private:
    [[nodiscard]] static bool selectable(const MenuItem &item);
    [[nodiscard]] std::vector<MenuItem> &items();
    [[nodiscard]] int first_selectable() const;
    void move(int direction);
    void change_tab(int direction);

    MenuView view_;
    std::uint64_t version_ = 0;
};

// Draws a MenuView as an image for the overlay path (premultiplied RGBA, R in the low byte). With `backdrop` the whole
// canvas is dimmed first (when the game picture behind is not blurred by the display backend).
class MenuRenderer {
public:
    explicit MenuRenderer(const std::filesystem::path &font_directory);
    ~MenuRenderer();

    // `scale` times the 480x272 canvas. Returns false when no font could be loaded.
    bool render(const MenuView &view, int scale, std::vector<std::uint32_t> &pixels, bool backdrop = true);

    [[nodiscard]] static int width(int scale) { return 480 * scale; }
    [[nodiscard]] static int height(int scale) { return 272 * scale; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::overlay
