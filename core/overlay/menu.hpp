#pragma once

// The in-game menu (docs/07-launcher-ux.md §3c): sections in a sidebar on the left, their rows in a panel on the right.
// Made for the mouse first (every option on screen, a click picks it), and still driven entirely by the keyboard or a
// controller (a focus ring shows where the keys act). This file is the model and the navigation; MenuRenderer draws it.
// Console- and game-independent: the program that owns the menu builds the sections and rows, reacts to what changed
// and rebuilds them.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mhf::overlay {

struct MenuItem {
    enum class Kind {
        Button,  // Accept or a click activates it
        Slider,  // `value` within min..max by `step`: Left/Right, a drag or the wheel
        Toggle,  // `value` 0 or 1, drawn as a switch; Accept or a click flips it
        Choice,  // `value` is an index into `options`, all of them shown (chips, or cards with `option_details`)
        Binding, // a button the player assigns: `text` is what it is set to, `value` 1 while it waits for a press
        Info,    // a note, wrapped; never selected
        Heading, // a title for the rows below, never selected
        Banner,  // a highlighted card: `label` small, `text` large, `actions` on its right
        Text,    // a text field: `label` above, `text` typed (`placeholder` when empty), `value` 1 while typing, `actions` after it
        Person,  // a friend: `label` the name, `detail` where they are, `value` 1 online (a green dot), `actions` on the right
    };
    Kind kind = Kind::Button;
    int id = 0;          // what the owner calls it
    std::string label;
    std::string detail;  // dim second line (optional)
    std::string text;    // Binding: the current assignment; Banner: the large text; Text: what is typed
    std::string placeholder;
    int value = 0;
    int min = 0, max = 100, step = 5;
    std::vector<std::string> options;
    std::vector<std::string> option_details; // Choice: a line under each option, which turns them into cards
    std::vector<std::string> actions;        // Banner, Text: small buttons on the right
    // Text: an on-screen keyboard that pops up under the field while this is not empty (for a controller), `key_columns`
    // keys a row; a last row with fewer keys spreads them across the width.
    std::vector<std::string> keys;
    int key_columns = 8;
    int primary_action = -1;                 // the action drawn as the main one (amber), -1 none
    bool danger = false; // a button that ends or deletes something
    bool enabled = true; // disabled rows are shown dim and skipped
};

struct MenuTab {
    std::string name;
    std::vector<MenuItem> items;
    std::string subtitle; // under the section's title
    std::string badge;    // a short tag next to its name in the sidebar ("Private")
};

struct MenuView {
    std::vector<MenuTab> tabs;
    int tab = 0;          // the section shown
    int selected = 0;     // row of that section
    int action = -1;      // the focused action of the selected row (Banner, Text), -1 the row itself
    int key = 0;          // the focused key of an open on-screen keyboard
    bool tabs_focused = false; // the focus is in the sidebar (Up/Down change the section)
    bool pointer = true;  // the mouse was used last: no focus ring, no key hints
    std::string title, subtitle;          // the sidebar's heading (the program, the game)
    std::string resume = "Back to the game"; // the main button at the bottom of the sidebar
    std::string hint;     // input hint, shown at the bottom while keys or a controller are used
    std::uint64_t version = 0; // changes whenever anything above changes

    [[nodiscard]] const std::vector<MenuItem> &items() const {
        static const std::vector<MenuItem> none;
        return tab >= 0 && tab < static_cast<int>(tabs.size()) ? tabs[static_cast<std::size_t>(tab)].items : none;
    }
};

// Where everything is on the 480x272 canvas, for the mouse (MenuRenderer::layout).
struct MenuLayout {
    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;
        [[nodiscard]] bool contains(float px, float py) const { return w > 0 && px >= x && px < x + w && py >= y && py < y + h; }
    };
    std::vector<Rect> tabs;   // the sidebar's sections
    std::vector<Rect> rows;   // one per row of the shown section; empty (w = 0) when it cannot be picked or is scrolled away
    std::vector<Rect> bars;   // the track of a slider row (empty for the other rows)
    std::vector<std::vector<Rect>> options; // a Choice row's chips or cards
    std::vector<std::vector<Rect>> actions; // a Banner or Text row's buttons
    std::vector<Rect> keys;   // the keys of an open on-screen keyboard
    Rect keypad;              // the panel around them
    int keypad_row = -1;      // the row it belongs to
    Rect resume, close;       // the sidebar's main button and the panel's ×

    struct Hit {
        enum class Kind { None, Tab, Row, Option, Action, Resume, Close, Key, Keypad } kind = Kind::None;
        int index = 0;       // the tab, or the row (Key, Keypad: the keyboard's row)
        int sub = 0;         // Option, Action, Key: which one in the row
        bool on_bar = false; // a slider's track is under the pointer
        float along = 0;     // across the slider track (on_bar) or across the row: 0..1
    };
    [[nodiscard]] Hit hit(float x, float y) const;
};

class Menu {
public:
    enum class Key { Up, Down, Left, Right, Accept, Back, PrevTab, NextTab };

    struct Event {
        enum class Kind { None, Activated, Changed, Back, Key } kind = Kind::None;
        int id = 0;     // the row concerned (Activated, Changed, Key)
        int value = 0;  // Changed: the new value; Key: the key pressed on the on-screen keyboard
        int action = -1; // Activated: the action pressed (Banner, Text), -1 the row itself
    };

    // Shows `view`. The section stays the one with the same name and the selection on the row with the same id when
    // there are such; otherwise the first section and its first row that can be selected.
    void set(MenuView view);
    [[nodiscard]] const MenuView &view() const { return view_; }

    // Applies a key; returns what the owner has to know about. Keys switch the menu out of pointer mode.
    Event press(Key key);
    // The mouse was used (on) or a key (off): the focus ring and the key hints show only for keys.
    void set_pointer(bool on);

    // The mouse. `row` and `tab` are indexes into the shown section / the sections.
    void hover_row(int row, int action = -1);  // the pointer is over a row (and one of its actions): it is selected
    void click_tab(int tab);
    Event click_row(int row, float along, bool on_bar); // a button is activated, a toggle flips, a slider jumps
    Event click_option(int row, int option);  // a Choice's chip or card
    Event click_action(int row, int action);  // a Banner's or Text's button
    Event drag_slider(int row, float along);  // the button is held on a slider's track
    Event scroll(int direction, int row);     // the wheel (+1 = up): over a slider it moves it, elsewhere it moves the selection
    void hover_key(int key);                  // the pointer is over a key of the on-screen keyboard
    Event click_key(int key);

    // The row of the shown section whose on-screen keyboard is open, -1 none. While one is, the keys move across it.
    [[nodiscard]] int keypad_row() const;

    // A row the focus can rest on (not a heading, a note, a disabled row or a banner without actions).
    [[nodiscard]] static bool selectable(const MenuItem &item);

private:
    Event set_value(MenuItem &item, int value);
    [[nodiscard]] static int first_action(const MenuItem &item);
    [[nodiscard]] std::vector<MenuItem> &items();
    [[nodiscard]] int first_selectable() const;
    void move(int direction);
    void change_tab(int direction);
    void focus_tabs();

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
    // At `width` x `height` pixels (the 480x272 canvas scaled by height / 272): the size the menu is shown at, so that the
    // display does not stretch it and its text stays sharp.
    bool render(const MenuView &view, int width, int height, std::vector<std::uint32_t> &pixels, bool backdrop = true);

    // Where the sections and rows of `view` are (the same positions render() draws at).
    [[nodiscard]] MenuLayout layout(const MenuView &view) const;

    [[nodiscard]] static int width(int scale) { return 480 * scale; }
    [[nodiscard]] static int height(int scale) { return 272 * scale; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::overlay
