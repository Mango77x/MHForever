#include "host/keymap.hpp"

namespace mhf::host {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::string KeyBindings::*member_for(std::string_view action) {
    if (action == "south") return &KeyBindings::south;
    if (action == "east") return &KeyBindings::east;
    if (action == "west") return &KeyBindings::west;
    if (action == "north") return &KeyBindings::north;
    if (action == "left_shoulder") return &KeyBindings::left_shoulder;
    if (action == "right_shoulder") return &KeyBindings::right_shoulder;
    if (action == "start") return &KeyBindings::start;
    if (action == "select") return &KeyBindings::select;
    if (action == "up") return &KeyBindings::up;
    if (action == "down") return &KeyBindings::down;
    if (action == "left") return &KeyBindings::left;
    if (action == "right") return &KeyBindings::right;
    if (action == "stick_up") return &KeyBindings::stick_up;
    if (action == "stick_down") return &KeyBindings::stick_down;
    if (action == "stick_left") return &KeyBindings::stick_left;
    if (action == "stick_right") return &KeyBindings::stick_right;
    return nullptr;
}

} // namespace

std::string *key_binding(KeyBindings &keys, std::string_view action) {
    const auto member = member_for(action);
    return member != nullptr ? &(keys.*member) : nullptr;
}

const std::string *key_binding(const KeyBindings &keys, std::string_view action) {
    const auto member = member_for(action);
    return member != nullptr ? &(keys.*member) : nullptr;
}

KeyBindings parse_keymap(std::string_view text) {
    KeyBindings keys;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view pair = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos) {
            if (std::string *slot = key_binding(keys, trim(pair.substr(0, equals)))) {
                *slot = std::string(trim(pair.substr(equals + 1)));
            }
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return keys;
}

std::string format_keymap(const KeyBindings &keys) {
    std::string out;
    for (const std::string_view action : kKeyActions) {
        if (!out.empty()) out.push_back(',');
        out.append(action);
        out.push_back('=');
        if (const std::string *value = key_binding(keys, action)) out.append(*value);
    }
    return out;
}

} // namespace mhf::host
