// sceUtility dialogs: message, on-screen keyboard, savedata and network configuration (docs/05-runtime-hle.md §5h).
//
// On the PSP the system draws these dialogs over the game while the game keeps calling *Update every frame. Here
// each dialog is a small state machine: *InitStart reads the request, *Update feeds it the player's input (the
// controller state plus keys/text the frontend forwards), and once the player answers the status moves to FINISHED.
// The frontend draws current_dialog(). With RuntimeOptions::interactive_dialogs off (headless runs, tests) every
// dialog answers at once, like a player who accepts.

#include "hle.hpp"
#include "adhocctl.hpp"
#include "mhf/psp/dialog.hpp"
#include "mhf/psp/host.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace mhf::psp::hle {
namespace {

enum DialogStatus : std::uint32_t { kNone = 0, kInit = 1, kRunning = 2, kFinished = 3, kShutdown = 4 };

constexpr std::uint32_t kResultOffset = 0x1C;     // pspUtilityDialogCommon::result
constexpr std::uint32_t kResultCancelled = 1;     // the player backed out
constexpr std::uint32_t kErrorBusy = 0x80110005u; // another dialog of this kind is running

struct Dialog {
    std::uint32_t status{kNone};
    std::uint32_t params{};
};
Dialog g_savedata, g_message, g_osk, g_netconf;

std::uint32_t get_status(Dialog &d) {
    const std::uint32_t s = d.status;
    if (s == kShutdown) d.status = kNone;
    if (s == kInit) d.status = kRunning;
    return s;
}

// ---------------------------------------------------------------- view and input shared by all dialogs

DialogView g_view;      // what the frontend draws
DialogView g_toast;     // corner notice, shown on its own until it expires
std::uint64_t g_toast_until = 0;
std::uint64_t g_version = 0;

void publish(DialogView view) {
    view.version = ++g_version;
    g_view = std::move(view);
}
void close_view() { publish({}); }
void show_toast(Runtime &rt, std::string title, std::string message) {
    g_toast = {};
    g_toast.kind = DialogView::Kind::Toast;
    g_toast.title = std::move(title);
    g_toast.message = std::move(message);
    g_toast.version = ++g_version;
    g_toast_until = rt.now_us() + 2'500'000u;
}

enum class Action { None, Confirm, Cancel, Up, Down, Left, Right, Delete, Space, Done };

// Turns the controller into presses: a button counts when it goes down; held directions repeat.
struct PadReader {
    std::uint32_t previous = ~0u; // everything "held" at open, so the press that opened a dialog is not reused
    std::uint64_t repeat_at = 0;

    void reset(Runtime &rt) { previous = controller_state(rt).buttons; }

    std::vector<Action> read(Runtime &rt) {
        const ControllerState &c = controller_state(rt);
        std::uint32_t now = c.buttons;
        // The left stick works like the d-pad.
        if (c.ly < 48) now |= buttons::kUp;
        if (c.ly > 208) now |= buttons::kDown;
        if (c.lx < 48) now |= buttons::kLeft;
        if (c.lx > 208) now |= buttons::kRight;
        const std::uint32_t pressed = now & ~previous;
        previous = now;
        std::vector<Action> out;
        const auto on = [&](std::uint32_t bit, Action a) {
            if ((pressed & bit) != 0) out.push_back(a);
        };
        on(buttons::kCross, Action::Confirm);
        on(buttons::kCircle, Action::Cancel);
        on(buttons::kSquare, Action::Delete);
        on(buttons::kTriangle, Action::Space);
        on(buttons::kStart, Action::Done);
        constexpr std::uint32_t kDirections = buttons::kUp | buttons::kDown | buttons::kLeft | buttons::kRight;
        const std::uint64_t t = rt.now_us();
        std::uint32_t dirs = pressed & kDirections;
        if (dirs != 0) {
            repeat_at = t + 400'000u;
        } else if ((now & kDirections) != 0 && t >= repeat_at) {
            dirs = now & kDirections;
            repeat_at = t + 90'000u;
        }
        if ((dirs & buttons::kUp) != 0) out.push_back(Action::Up);
        if ((dirs & buttons::kDown) != 0) out.push_back(Action::Down);
        if ((dirs & buttons::kLeft) != 0) out.push_back(Action::Left);
        if ((dirs & buttons::kRight) != 0) out.push_back(Action::Right);
        return out;
    }
};

bool interactive(Runtime &rt) { return rt.options().interactive_dialogs; }

std::string read_utf16(Runtime &rt, std::uint32_t address, std::uint32_t max) {
    std::string out;
    if (address == 0) return out;
    for (std::uint32_t i = 0; i < max; ++i) {
        const std::uint32_t c = rt.memory().load16(address + i * 2u);
        if (c == 0) break;
        // UTF-16 → UTF-8 (BMP only; surrogates are shown as '?').
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c >= 0xD800 && c < 0xE000) {
            out.push_back('?');
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// ---------------------------------------------------------------- message dialog

constexpr std::uint32_t kMsgMode = 0x34;    // 0 = error code, 1 = text
constexpr std::uint32_t kMsgError = 0x38;
constexpr std::uint32_t kMsgText = 0x3C;    // char[512], UTF-8
constexpr std::uint32_t kMsgOptions = 0x23C;
constexpr std::uint32_t kMsgButton = 0x240; // answer: 1 = yes/OK, 2 = no, 3 = back
constexpr std::uint32_t kOptYesNo = 0x10, kOptOk = 0x20, kOptNoBack = 0x80, kOptDefaultNo = 0x100;

struct MessageState {
    bool yes_no = false, ok = false, can_back = true;
    int selected = 0;
    PadReader pad;
} g_msg;

void message_view() {
    DialogView v = g_view;
    v.selected = g_msg.selected;
    publish(std::move(v));
}

void message_answer(Runtime &rt, std::uint32_t button) {
    rt.memory().store32(g_message.params + kMsgButton, button);
    g_message.status = kFinished;
    close_view();
}

void message_action(Runtime &rt, Action a) {
    const int count = g_msg.yes_no ? 2 : 1;
    switch (a) {
    case Action::Left:
    case Action::Up:
        if (g_msg.selected > 0) { --g_msg.selected; message_view(); }
        break;
    case Action::Right:
    case Action::Down:
        if (g_msg.selected + 1 < count) { ++g_msg.selected; message_view(); }
        break;
    case Action::Confirm:
    case Action::Done:
        if (g_msg.yes_no) message_answer(rt, g_msg.selected == 0 ? 1u : 2u);
        else message_answer(rt, g_msg.ok ? 1u : 3u);
        break;
    case Action::Cancel:
        if (g_msg.can_back) message_answer(rt, 3u);
        break;
    default: break;
    }
}

void MsgDialogInitStart(Runtime &rt, Context &ctx) {
    if (g_message.status != kNone) {
        ret(ctx, kErrorBusy);
        return;
    }
    auto &mem = rt.memory();
    g_message.params = arg(ctx, 0);
    const std::uint32_t p = g_message.params;
    const std::uint32_t mode = mem.load32(p + kMsgMode), options = mem.load32(p + kMsgOptions);
    const std::uint32_t error = mem.load32(p + kMsgError);
    std::string text = read_string(rt, p + kMsgText, 512);
    std::fprintf(stderr, "[hle] message dialog (mode %u, options %X, error %08X): \"%s\"\n", mode, options, error,
                 text.c_str());
    mem.store32(p + kResultOffset, 0);
    g_msg = {};
    g_msg.yes_no = (options & kOptYesNo) != 0;
    g_msg.ok = !g_msg.yes_no && (options & kOptOk) != 0;
    g_msg.can_back = (options & kOptNoBack) == 0;
    g_msg.selected = g_msg.yes_no && (options & kOptDefaultNo) != 0 ? 1 : 0;
    g_message.status = kInit;
    if (!interactive(rt)) {
        // A player who accepts: "yes" to questions, OK to notices, "back" when that is the only button.
        mem.store32(p + kMsgButton, g_msg.yes_no || g_msg.ok ? 1u : 3u);
        ret(ctx, 0);
        return;
    }
    DialogView v;
    v.kind = DialogView::Kind::Message;
    if (mode == 0) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "An error occurred (%08X).", error);
        v.message = buf;
        v.error = true;
    } else {
        v.message = std::move(text);
    }
    if (g_msg.yes_no) v.buttons = {"Yes", "No"};
    else v.buttons = {g_msg.ok ? "OK" : "Back"};
    v.selected = g_msg.selected;
    v.hint = g_msg.can_back ? "Confirm  \xC2\xB7  Back" : "Confirm";
    publish(std::move(v));
    g_msg.pad.reset(rt);
    ret(ctx, 0);
}

void MsgDialogUpdate(Runtime &rt, Context &ctx) {
    if (g_message.status == kInit || g_message.status == kRunning) {
        if (!interactive(rt)) {
            g_message.status = kFinished;
        } else {
            for (const Action a : g_msg.pad.read(rt)) {
                if (g_message.status != kFinished) message_action(rt, a);
            }
        }
    }
    ret(ctx, 0);
}
void MsgDialogGetStatus(Runtime &, Context &ctx) { ret(ctx, get_status(g_message)); }
void MsgDialogShutdownStart(Runtime &, Context &ctx) {
    if (g_message.status == kInit || g_message.status == kRunning) close_view(); // the game closed it itself
    g_message.status = kShutdown;
    ret(ctx, 0);
}

// ---------------------------------------------------------------- on-screen keyboard

constexpr std::uint32_t kOskFieldCount = 0x30, kOskFields = 0x34, kOskFieldSize = 0x34;
constexpr std::uint32_t kFieldDesc = 28, kFieldInText = 32, kFieldCapacity = 36, kFieldOutText = 40, kFieldResult = 44,
                        kFieldLimit = 48;
constexpr std::uint32_t kOskUnchanged = 0, kOskCancelled = 1, kOskChanged = 2;

const std::vector<std::string> kGrid = {
    "1234567890", "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-_.!", "abcdefghij", "klmnopqrst", "uvwxyz'&?#", "\x01\x02\x03",
};

struct OskState {
    std::uint32_t field = 0, count = 0;
    std::u16string initial; // the field's text when it opened
    std::string text;       // ASCII
    std::size_t max = 0;
    std::string desc;
    int row = 1, col = 0;
    PadReader pad;
} g_kb;

std::uint32_t osk_field_address(Runtime &rt, std::uint32_t index) {
    return rt.memory().load32(g_osk.params + kOskFields) + index * kOskFieldSize;
}

void osk_view() {
    DialogView v;
    v.kind = DialogView::Kind::Keyboard;
    v.title = g_kb.desc.empty() ? "Enter text" : g_kb.desc;
    v.text = g_kb.text;
    v.max_length = g_kb.max;
    v.grid = kGrid;
    v.grid_row = g_kb.row;
    v.grid_col = g_kb.col;
    v.hint = "Type or pick keys  \xC2\xB7  Enter: done  \xC2\xB7  Esc: cancel";
    publish(std::move(v));
}

// Opens field `index` for editing (or finishes the dialog after the last one).
void osk_open_field(Runtime &rt, std::uint32_t index) {
    auto &mem = rt.memory();
    if (index >= g_kb.count) {
        g_osk.status = kFinished;
        close_view();
        return;
    }
    g_kb.field = index;
    const std::uint32_t f = osk_field_address(rt, index);
    const std::uint32_t intext = mem.load32(f + kFieldInText), capacity = mem.load32(f + kFieldCapacity);
    const std::uint32_t limit = mem.load32(f + kFieldLimit);
    g_kb.initial.clear();
    for (std::uint32_t j = 0; intext != 0 && j < 512; ++j) {
        const std::uint16_t c = mem.load16(intext + j * 2u);
        if (c == 0) break;
        g_kb.initial.push_back(static_cast<char16_t>(c));
    }
    g_kb.max = 512;
    if (limit != 0) g_kb.max = std::min<std::size_t>(g_kb.max, limit);
    if (capacity != 0) g_kb.max = std::min<std::size_t>(g_kb.max, capacity - 1u);
    g_kb.text.clear();
    for (const char16_t c : g_kb.initial) g_kb.text.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    if (g_kb.text.empty()) g_kb.text = rt.options().osk_default_text;
    if (g_kb.text.size() > g_kb.max) g_kb.text.resize(g_kb.max);
    g_kb.desc = read_utf16(rt, mem.load32(f + kFieldDesc), 64);
    g_kb.row = 1;
    g_kb.col = 0;
    std::fprintf(stderr, "[hle] on-screen keyboard \"%s\" (up to %zu characters)\n", g_kb.desc.c_str(), g_kb.max);
}

void osk_store(Runtime &rt, const std::u16string &text, std::uint32_t result) {
    auto &mem = rt.memory();
    const std::uint32_t f = osk_field_address(rt, g_kb.field);
    const std::uint32_t outtext = mem.load32(f + kFieldOutText);
    const std::size_t n = std::min(text.size(), g_kb.max);
    if (outtext != 0) {
        for (std::size_t j = 0; j < n; ++j) mem.store16(outtext + static_cast<std::uint32_t>(j) * 2u, text[j]);
        mem.store16(outtext + static_cast<std::uint32_t>(n) * 2u, 0);
    }
    mem.store32(f + kFieldResult, result);
}

void osk_finish_field(Runtime &rt, bool cancelled) {
    if (cancelled) {
        osk_store(rt, g_kb.initial, kOskCancelled);
    } else {
        std::u16string out(g_kb.text.begin(), g_kb.text.end());
        osk_store(rt, out, out == g_kb.initial ? kOskUnchanged : kOskChanged);
        std::fprintf(stderr, "[hle] on-screen keyboard -> \"%s\"\n", g_kb.text.c_str());
    }
    osk_open_field(rt, g_kb.field + 1);
    if (g_osk.status != kFinished) osk_view();
}

void osk_type(char c) {
    if (g_kb.text.size() < g_kb.max) g_kb.text.push_back(c);
}

void osk_action(Runtime &rt, Action a) {
    const auto row_len = [](int r) { return static_cast<int>(kGrid[static_cast<std::size_t>(r)].size()); };
    const auto move_row = [&](int to) {
        const int from_len = row_len(g_kb.row), to_len = row_len(to);
        if (from_len != to_len) g_kb.col = std::min(to_len - 1, (g_kb.col * to_len + from_len / 2) / from_len);
        g_kb.row = to;
    };
    const int rows = static_cast<int>(kGrid.size());
    switch (a) {
    case Action::Up: move_row((g_kb.row + rows - 1) % rows); break;
    case Action::Down: move_row((g_kb.row + 1) % rows); break;
    case Action::Left: g_kb.col = (g_kb.col + row_len(g_kb.row) - 1) % row_len(g_kb.row); break;
    case Action::Right: g_kb.col = (g_kb.col + 1) % row_len(g_kb.row); break;
    case Action::Delete:
        if (!g_kb.text.empty()) g_kb.text.pop_back();
        break;
    case Action::Space: osk_type(' '); break;
    case Action::Done: osk_finish_field(rt, false); return;
    case Action::Cancel: osk_finish_field(rt, true); return;
    case Action::Confirm: {
        const char key = kGrid[static_cast<std::size_t>(g_kb.row)][static_cast<std::size_t>(g_kb.col)];
        if (key == '\x01') osk_type(' ');
        else if (key == '\x02') { if (!g_kb.text.empty()) g_kb.text.pop_back(); }
        else if (key == '\x03') { osk_finish_field(rt, false); return; }
        else osk_type(key);
        break;
    }
    default: return;
    }
    osk_view();
}

void OskInitStart(Runtime &rt, Context &ctx) {
    if (g_osk.status != kNone) {
        ret(ctx, kErrorBusy);
        return;
    }
    auto &mem = rt.memory();
    g_osk.params = arg(ctx, 0);
    mem.store32(g_osk.params + kResultOffset, 0);
    g_kb = {};
    g_kb.count = mem.load32(g_osk.params + kOskFields) == 0 ? 0 : std::min<std::uint32_t>(mem.load32(g_osk.params + kOskFieldCount), 16);
    g_osk.status = kInit;
    osk_open_field(rt, 0);
    if (!interactive(rt)) {
        // Every field gets its text as if typed and confirmed.
        while (g_osk.status != kFinished) osk_finish_field(rt, false);
        g_osk.status = kInit;
    } else if (g_osk.status != kFinished) {
        osk_view();
        g_kb.pad.reset(rt);
    }
    ret(ctx, 0);
}
void OskUpdate(Runtime &rt, Context &ctx) {
    if (g_osk.status == kInit || g_osk.status == kRunning) {
        if (!interactive(rt) || g_kb.field >= g_kb.count) {
            g_osk.status = kFinished;
        } else {
            for (const Action a : g_kb.pad.read(rt)) {
                if (g_osk.status == kInit || g_osk.status == kRunning) osk_action(rt, a);
            }
        }
    }
    ret(ctx, 0);
}
void OskGetStatus(Runtime &, Context &ctx) { ret(ctx, get_status(g_osk)); }
void OskShutdownStart(Runtime &, Context &ctx) {
    if (g_osk.status == kInit || g_osk.status == kRunning) close_view();
    g_osk.status = kShutdown;
    ret(ctx, 0);
}

// ---------------------------------------------------------------- savedata dialog

struct SaveState {
    enum class Step { Idle, Choose, Confirm, Working } step = Step::Idle;
    SavedataPlan plan;
    int selected = 0;      // list row, or button in Confirm (0 = Yes)
    std::optional<std::string> chosen;
    std::uint64_t done_at = 0; // Working: when the progress view may close
    PadReader pad;
} g_save;

const char *verb(SavedataPlan::Action a) {
    return a == SavedataPlan::Action::Save ? "Save" : a == SavedataPlan::Action::Load ? "Load" : "Delete";
}

void save_list_view() {
    DialogView v;
    v.kind = DialogView::Kind::List;
    v.title = std::string(verb(g_save.plan.action)) + " game";
    for (const auto &s : g_save.plan.slots) {
        DialogView::Item item;
        if (s.exists) {
            item.title = s.title.empty() ? s.name : s.title;
            item.detail = s.detail;
            item.date = s.date;
        } else {
            item.title = "New save";
            item.detail = g_save.plan.new_title;
            item.is_new = true;
        }
        v.items.push_back(std::move(item));
    }
    v.selected = g_save.selected;
    v.hint = "Choose  \xC2\xB7  Back";
    publish(std::move(v));
}

void save_confirm_view(const SavedataSlot &slot) {
    DialogView v;
    v.kind = DialogView::Kind::Message;
    const auto a = g_save.plan.action;
    v.title = std::string(verb(a)) + " game";
    if (a == SavedataPlan::Action::Save) v.message = slot.exists ? "Overwrite this saved data?" : "Save the game?";
    else if (a == SavedataPlan::Action::Load) v.message = "Load this saved data?";
    else v.message = "Delete this saved data? This cannot be undone.";
    v.error = a == SavedataPlan::Action::Delete;
    DialogView::Item item;
    item.title = slot.exists ? (slot.title.empty() ? slot.name : slot.title) : g_save.plan.new_title;
    item.detail = slot.exists ? slot.detail : g_save.plan.new_detail;
    item.date = slot.date;
    item.is_new = !slot.exists;
    v.items.push_back(std::move(item));
    v.buttons = {"Yes", "No"};
    v.selected = g_save.selected;
    v.hint = "Confirm  \xC2\xB7  Back";
    publish(std::move(v));
}

void save_finish(Runtime &rt) {
    g_savedata.status = kFinished;
    g_save.step = SaveState::Step::Idle;
    close_view();
    (void)rt;
}

void save_cancel(Runtime &rt) {
    rt.memory().store32(g_savedata.params + kResultOffset, kResultCancelled);
    save_finish(rt);
}

// Runs the request and shows the progress view for a moment (the PSP shows it while writing).
void save_run(Runtime &rt) {
    const auto a = g_save.plan.action;
    DialogView v;
    v.kind = DialogView::Kind::Progress;
    v.title = a == SavedataPlan::Action::Save ? "Saving..." : a == SavedataPlan::Action::Load ? "Loading..." : "Deleting...";
    v.message = a == SavedataPlan::Action::Save ? "Do not close the game." : "";
    publish(std::move(v));
    const std::uint32_t result = execute_savedata(rt, g_savedata.params, g_save.chosen);
    g_save.step = SaveState::Step::Working;
    g_save.done_at = rt.now_us() + 600'000u;
    if (result == 0) {
        show_toast(rt, a == SavedataPlan::Action::Save ? "Game saved" : a == SavedataPlan::Action::Load ? "Game loaded" : "Data deleted",
                   g_save.plan.new_title);
    }
}

const SavedataSlot &save_target() {
    const auto &slots = g_save.plan.slots;
    return slots[static_cast<std::size_t>(std::clamp(g_save.selected, 0, static_cast<int>(slots.size()) - 1))];
}

void save_action(Runtime &rt, Action a) {
    switch (g_save.step) {
    case SaveState::Step::Choose: {
        const int count = static_cast<int>(g_save.plan.slots.size());
        if (a == Action::Up && g_save.selected > 0) { --g_save.selected; save_list_view(); }
        else if (a == Action::Down && g_save.selected + 1 < count) { ++g_save.selected; save_list_view(); }
        else if (a == Action::Cancel) save_cancel(rt);
        else if (a == Action::Confirm || a == Action::Done) {
            const SavedataSlot &slot = save_target();
            g_save.chosen = slot.name;
            if (g_save.plan.action == SavedataPlan::Action::Load) {
                save_run(rt); // choosing what to load is the confirmation
            } else {
                const SavedataSlot copy = slot;
                g_save.plan.slots = {copy};
                g_save.selected = 0;
                g_save.step = SaveState::Step::Confirm;
                save_confirm_view(copy);
            }
        }
        break;
    }
    case SaveState::Step::Confirm:
        if ((a == Action::Left || a == Action::Up) && g_save.selected > 0) { g_save.selected = 0; save_confirm_view(save_target()); }
        else if ((a == Action::Right || a == Action::Down) && g_save.selected == 0) {
            g_save.selected = 1;
            DialogView v = g_view;
            v.selected = 1;
            publish(std::move(v));
        } else if (a == Action::Cancel) save_cancel(rt);
        else if (a == Action::Confirm || a == Action::Done) {
            if (g_save.selected == 0) save_run(rt);
            else save_cancel(rt);
        }
        break;
    default: break;
    }
}

void SavedataInitStart(Runtime &rt, Context &ctx) {
    if (g_savedata.status != kNone) {
        ret(ctx, kErrorBusy);
        return;
    }
    g_savedata.params = arg(ctx, 0);
    g_savedata.status = kInit;
    g_save = {};
    SavedataPlan plan = interactive(rt) ? savedata_plan(rt, g_savedata.params) : SavedataPlan{};
    const bool missing = plan.ui == SavedataPlan::Ui::Confirm && plan.action != SavedataPlan::Action::Save &&
                         (plan.slots.empty() || !plan.slots[0].exists);
    if (plan.ui == SavedataPlan::Ui::None || missing ||
        (plan.ui == SavedataPlan::Ui::List && plan.slots.empty())) {
        // Sizes, listings, or nothing to load/delete: no question to ask, the request answers for itself.
        execute_savedata(rt, g_savedata.params);
        ret(ctx, 0);
        return;
    }
    if (plan.ui == SavedataPlan::Ui::Toast) {
        const std::uint32_t result = execute_savedata(rt, g_savedata.params);
        if (result == 0 && plan.action == SavedataPlan::Action::Save) show_toast(rt, "Saved", plan.new_title);
        if (result == 0 && plan.action == SavedataPlan::Action::Load) show_toast(rt, "Loaded", plan.new_title);
        ret(ctx, 0);
        return;
    }
    g_save.plan = std::move(plan);
    g_save.pad.reset(rt);
    if (g_save.plan.ui == SavedataPlan::Ui::List) {
        g_save.step = SaveState::Step::Choose;
        g_save.selected = g_save.plan.focus;
        save_list_view();
    } else {
        g_save.step = SaveState::Step::Confirm;
        save_confirm_view(g_save.plan.slots[0]);
    }
    ret(ctx, 0);
}

void SavedataUpdate(Runtime &rt, Context &ctx) {
    if (g_savedata.status == kInit || g_savedata.status == kRunning) {
        if (g_save.step == SaveState::Step::Idle) {
            g_savedata.status = kFinished;
        } else if (g_save.step == SaveState::Step::Working) {
            if (rt.now_us() >= g_save.done_at) save_finish(rt);
        } else {
            for (const Action a : g_save.pad.read(rt)) {
                if (g_save.step == SaveState::Step::Choose || g_save.step == SaveState::Step::Confirm) save_action(rt, a);
            }
        }
    }
    ret(ctx, 0);
}
void SavedataGetStatus(Runtime &, Context &ctx) { ret(ctx, get_status(g_savedata)); }
void SavedataShutdownStart(Runtime &, Context &ctx) {
    if (g_save.step != SaveState::Step::Idle) close_view();
    g_save.step = SaveState::Step::Idle;
    g_savedata.status = kShutdown;
    ret(ctx, 0);
}

// ---------------------------------------------------------------- network configuration (no UI: always "connected")

// SceUtilityNetconfParam: the common header (0x30 bytes), then the action and a pointer to {groupName[8], timeout}.
constexpr std::uint32_t kNetconfAction = 0x30, kNetconfData = 0x34;
constexpr std::uint32_t kNetconfConnectAdhoc = 2, kNetconfCreateAdhoc = 4, kNetconfJoinAdhoc = 5;
constexpr std::uint32_t kDialogResultAbort = 1;
constexpr std::uint64_t kNetconfAdhocTimeoutUs = 15'000'000; // as long as the ad hoc servers wait for a client to join
bool g_netconf_adhoc = false;
std::uint64_t g_netconf_started = 0;

void NetconfInitStart(Runtime &rt, Context &ctx) {
    g_netconf.params = arg(ctx, 0);
    rt.memory().store32(g_netconf.params + kResultOffset, 0);
    g_netconf.status = kInit;
    g_netconf_adhoc = false;
    const std::uint32_t action = rt.memory().load32(g_netconf.params + kNetconfAction);
    if (action == kNetconfConnectAdhoc || action == kNetconfCreateAdhoc || action == kNetconfJoinAdhoc) {
        // Joining a group of the ad hoc network: the dialog stays until the library is in it (or gives up).
        const std::uint32_t data = rt.memory().load32(g_netconf.params + kNetconfData);
        adhoc::GroupName group{};
        for (std::uint32_t i = 0; data != 0 && i < group.size(); ++i) group[i] = static_cast<char>(rt.memory().load8(data + i));
        auto &library = net_adhocctl(rt);
        g_netconf_adhoc = true;
        g_netconf_started = rt.now_us();
        net_trace(("network configuration: join the ad hoc group " + adhoc::printable(group) + " (action " +
                   std::to_string(action) + ")").c_str());
        if (!library.initialized() || !library.online() || library.connect(group) != 0) {
            // No ad hoc library, no server to reach, or already in a group: the dialog reports failure at its first update.
            g_netconf_started = 0;
        }
    }
    ret(ctx, 0);
}
void NetconfUpdate(Runtime &rt, Context &ctx) {
    if (g_netconf.status == kRunning || g_netconf.status == kInit) {
        if (!g_netconf_adhoc) {
            g_netconf.status = kFinished;
        } else {
            auto &library = net_adhocctl(rt);
            if (library.state() == adhoc::State::Connected) {
                net_trace("network configuration: connected");
                g_netconf.status = kFinished;
            } else if (g_netconf_started == 0 || rt.now_us() - g_netconf_started > kNetconfAdhocTimeoutUs) {
                net_trace("network configuration: could not connect");
                rt.memory().store32(g_netconf.params + kResultOffset, kDialogResultAbort);
                g_netconf.status = kFinished;
            }
        }
    }
    ret(ctx, 0);
}
void NetconfGetStatus(Runtime &, Context &ctx) { ret(ctx, get_status(g_netconf)); }
void NetconfShutdownStart(Runtime &, Context &ctx) {
    g_netconf.status = kShutdown;
    ret(ctx, 0);
}

const HleFunction kFunctions[] = {
    {"sceUtility", 0x50C4CD57u, "sceUtilitySavedataInitStart", &SavedataInitStart},
    {"sceUtility", 0xD4B95FFBu, "sceUtilitySavedataUpdate", &SavedataUpdate},
    {"sceUtility", 0x8874DBE0u, "sceUtilitySavedataGetStatus", &SavedataGetStatus},
    {"sceUtility", 0x9790B33Cu, "sceUtilitySavedataShutdownStart", &SavedataShutdownStart},
    {"sceUtility", 0x2AD8E239u, "sceUtilityMsgDialogInitStart", &MsgDialogInitStart},
    {"sceUtility", 0x95FC253Bu, "sceUtilityMsgDialogUpdate", &MsgDialogUpdate},
    {"sceUtility", 0x9A1C91D7u, "sceUtilityMsgDialogGetStatus", &MsgDialogGetStatus},
    {"sceUtility", 0x67AF3428u, "sceUtilityMsgDialogShutdownStart", &MsgDialogShutdownStart},
    {"sceUtility", 0xF6269B82u, "sceUtilityOskInitStart", &OskInitStart},
    {"sceUtility", 0x4B85C861u, "sceUtilityOskUpdate", &OskUpdate},
    {"sceUtility", 0xF3F76017u, "sceUtilityOskGetStatus", &OskGetStatus},
    {"sceUtility", 0x3DFAEBA9u, "sceUtilityOskShutdownStart", &OskShutdownStart},
    {"sceUtility", 0x4DB1E739u, "sceUtilityNetconfInitStart", &NetconfInitStart},
    {"sceUtility", 0x91E70E35u, "sceUtilityNetconfUpdate", &NetconfUpdate},
    {"sceUtility", 0x6332AA39u, "sceUtilityNetconfGetStatus", &NetconfGetStatus},
    {"sceUtility", 0xF88155F6u, "sceUtilityNetconfShutdownStart", &NetconfShutdownStart},
};

} // namespace

std::span<const HleFunction> utility_functions() { return kFunctions; }

} // namespace mhf::psp::hle

namespace mhf::psp {

const DialogView &current_dialog(Runtime &rt) {
    using namespace hle;
    if (g_view.kind != DialogView::Kind::None) return g_view;
    if (g_toast.kind != DialogView::Kind::None && rt.now_us() >= g_toast_until) {
        g_toast = {};
        g_toast.version = ++g_version;
    }
    return g_toast.kind != DialogView::Kind::None ? g_toast : g_view;
}

void dialog_key(Runtime &rt, DialogKey key) {
    using namespace hle;
    const bool keyboard = g_view.kind == DialogView::Kind::Keyboard;
    Action a = Action::None;
    switch (key) {
    case DialogKey::Enter: a = keyboard ? Action::Done : Action::Confirm; break;
    case DialogKey::Escape: a = Action::Cancel; break;
    case DialogKey::Backspace: a = keyboard ? Action::Delete : Action::None; break;
    case DialogKey::Left: a = Action::Left; break;
    case DialogKey::Right: a = Action::Right; break;
    case DialogKey::Up: a = Action::Up; break;
    case DialogKey::Down: a = Action::Down; break;
    }
    if (a == Action::None) return;
    if (keyboard && (g_osk.status == kInit || g_osk.status == kRunning)) {
        // Arrow keys move the text caret on a PC keyboard; here they move over the key grid like the d-pad.
        osk_action(rt, a);
    } else if (g_view.kind == DialogView::Kind::Message && (g_message.status == kInit || g_message.status == kRunning) &&
               g_save.step == SaveState::Step::Idle) {
        message_action(rt, a);
    } else if (g_save.step == SaveState::Step::Choose || g_save.step == SaveState::Step::Confirm) {
        save_action(rt, a);
    }
}

void dialog_text(Runtime &, const std::string &utf8) {
    using namespace hle;
    if (g_view.kind != DialogView::Kind::Keyboard || !(g_osk.status == kInit || g_osk.status == kRunning)) return;
    for (const char c : utf8) {
        if (c >= 0x20 && c < 0x7F) osk_type(c); // the game's name fields are ASCII; other characters are dropped
    }
    osk_view();
}

bool dialog_wants_text(Runtime &) {
    return hle::g_view.kind == DialogView::Kind::Keyboard;
}

} // namespace mhf::psp
