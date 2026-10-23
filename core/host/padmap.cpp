#include "host/padmap.hpp"

#include <utility>

namespace mhf::host {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

constexpr std::pair<std::string_view, std::string_view> kSourceLabels[] = {
    {"south", "A (Cross)"},       {"east", "B (Circle)"},      {"west", "X (Square)"},       {"north", "Y (Triangle)"},
    {"left_shoulder", "LB (L1)"}, {"right_shoulder", "RB (R1)"}, {"left_trigger", "LT (L2)"}, {"right_trigger", "RT (R2)"},
    {"back", "Back (Select)"},    {"start", "Start"},          {"guide", "Guide"},           {"left_stick", "L3"},
    {"right_stick", "R3"},        {"dpad_up", "D-pad Up"},     {"dpad_down", "D-pad Down"},  {"dpad_left", "D-pad Left"},
    {"dpad_right", "D-pad Right"},
};

std::string PadBindings::*member_for(std::string_view action) {
    if (action == "south") return &PadBindings::south;
    if (action == "east") return &PadBindings::east;
    if (action == "west") return &PadBindings::west;
    if (action == "north") return &PadBindings::north;
    if (action == "left_shoulder") return &PadBindings::left_shoulder;
    if (action == "right_shoulder") return &PadBindings::right_shoulder;
    if (action == "start") return &PadBindings::start;
    if (action == "select") return &PadBindings::select;
    if (action == "up") return &PadBindings::up;
    if (action == "down") return &PadBindings::down;
    if (action == "left") return &PadBindings::left;
    if (action == "right") return &PadBindings::right;
    if (action == "stick") return &PadBindings::stick;
    return nullptr;
}

} // namespace

std::string pad_label(std::string_view binding) {
    if (binding.empty()) return "Not set";
    std::string out;
    std::size_t start = 0;
    while (start <= binding.size()) {
        const std::size_t plus = binding.find('+', start);
        const std::string_view part = binding.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
        std::string label(part);
        for (const auto &[name, text] : kSourceLabels) {
            if (part == name) label = std::string(text);
        }
        if (!out.empty()) out.append(" + ");
        out.append(label);
        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    return out;
}

std::string *pad_binding(PadBindings &pad, std::string_view action) {
    const auto member = member_for(action);
    return member != nullptr ? &(pad.*member) : nullptr;
}

const std::string *pad_binding(const PadBindings &pad, std::string_view action) {
    const auto member = member_for(action);
    return member != nullptr ? &(pad.*member) : nullptr;
}

PadBindings parse_padmap(std::string_view text) {
    PadBindings pad;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view pair = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos) {
            if (std::string *slot = pad_binding(pad, trim(pair.substr(0, equals)))) {
                *slot = std::string(trim(pair.substr(equals + 1)));
            }
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return pad;
}

std::string format_padmap(const PadBindings &pad) {
    std::string out;
    for (const std::string_view action : kPadActions) {
        if (!out.empty()) out.push_back(',');
        out.append(action);
        out.push_back('=');
        if (const std::string *value = pad_binding(pad, action)) out.append(*value);
    }
    return out;
}

} // namespace mhf::host
