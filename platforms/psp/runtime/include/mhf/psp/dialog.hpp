#pragma once

// System dialogs (sceUtility): what the frontend draws over the game and the input it feeds back
// (docs/05-runtime-hle.md §5h). The PSP draws these itself on top of the game; here the runtime keeps the dialog's
// state and the frontend renders a view of it. Everything runs on the emulation thread.

#include "mhf/psp/runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mhf::psp {

struct DialogView {
    enum class Kind {
        None,
        Message,  // text + buttons (Yes/No, OK or Back)
        Progress, // "Saving..." while a save/load runs; no input
        Keyboard, // on-screen keyboard (OSK)
        List,     // save slots to choose from
        Toast,    // small corner notice (autosave/autoload), no input
    };
    Kind kind = Kind::None;
    std::string title;
    std::string message;
    std::vector<std::string> buttons;
    int selected = 0;
    bool error = false; // error-style message (warning accent)

    // Keyboard.
    std::string text;           // UTF-8, what the player typed so far
    std::size_t max_length = 0; // characters
    int grid_row = 0, grid_col = 0;
    std::vector<std::string> grid; // rows of keys; "\x01" = space, "\x02" = delete, "\x03" = done

    // List.
    struct Item {
        std::string title, detail, date;
        bool is_new = false;
    };
    std::vector<Item> items;

    std::string hint;      // input hint line
    std::uint64_t version = 0; // changes whenever anything above changes
};

// The dialog to draw now (Kind::None when no dialog is open).
[[nodiscard]] const DialogView &current_dialog(Runtime &rt);

// Keyboard input while a dialog is open (physical keyboard; the gamepad goes through the controller state).
enum class DialogKey { Enter, Escape, Backspace, Left, Right, Up, Down };
void dialog_key(Runtime &rt, DialogKey key);
void dialog_text(Runtime &rt, const std::string &utf8);
// Whether the frontend should collect typed text (the on-screen keyboard is open).
[[nodiscard]] bool dialog_wants_text(Runtime &rt);

} // namespace mhf::psp
