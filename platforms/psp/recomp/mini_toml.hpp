#pragma once

// Just enough TOML for our own manifests (overlays.toml, symbols.toml): `[table]`, `[[array-of-tables]]`,
// `key = value` with strings, integers (decimal or 0x hex) and booleans, `#` comments. No nesting, no inline tables.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mhf::psp::recomp::toml {

using Table = std::map<std::string, std::string>; // values keep their text, strings without quotes

struct Document {
    std::map<std::string, Table> tables;                    // "" is the root table
    std::map<std::string, std::vector<Table>> table_arrays; // [[name]]
};

inline std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

inline Document parse_file(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    Document doc;
    Table *current = &doc.tables[""];
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        // Strip comments outside strings.
        bool quoted = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') quoted = !quoted;
            if (line[i] == '#' && !quoted) {
                line.resize(i);
                break;
            }
        }
        line = trim(line);
        if (line.empty()) continue;
        if (line.rfind("[[", 0) == 0) {
            const auto name = trim(line.substr(2, line.find("]]") - 2));
            current = &doc.table_arrays[name].emplace_back();
            continue;
        }
        if (line.front() == '[') {
            current = &doc.tables[trim(line.substr(1, line.find(']') - 1))];
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error(path.string() + ":" + std::to_string(number) + ": expected key = value");
        }
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        (*current)[trim(line.substr(0, eq))] = value;
    }
    return doc;
}

inline std::uint64_t to_integer(const std::string &text) {
    std::string t;
    for (const char c : text) {
        if (c != '_') t.push_back(c);
    }
    return std::stoull(t, nullptr, t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0 ? 16 : 10);
}

inline std::optional<std::string> get(const Table &table, const std::string &key) {
    const auto it = table.find(key);
    if (it == table.end()) return std::nullopt;
    return it->second;
}

} // namespace mhf::psp::recomp::toml
