// Region data (recomp/emitter.*): the entries of the active region map with the names only other region maps list,
// the header written from them (address or std::nullopt), the file left as it is when nothing changed, and a name that
// is not a C++ identifier refused. Everything is written in a temporary folder from synthetic maps.

#include "emitter.hpp"

#include "check.hpp"

#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using mhf::psp::recomp::RegionData;
using mhf::psp::recomp::emit_region_data;
using mhf::psp::recomp::region_data_entries;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("mhf_region_data_test_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path &file, const std::string &text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

} // namespace

int main() {
    TempDir dir;

    // The active region maps one entry; another region maps it too and one more; a third folder has no map.
    const auto active = dir.path / "regions" / "active_region" / "symbols.toml";
    write(active, "[functions]\nentry_point = 0x08804000\n\n[data]\nmapped_entry = 0x08800100 # u32\n");
    write(dir.path / "regions" / "other_region" / "symbols.toml",
          "[data]\nmapped_entry = 0x08800200\nunmapped_entry = 0x08800300\n");
    fs::create_directories(dir.path / "regions" / "region_without_map");
    const RegionData entries = region_data_entries(active);
    MHF_CHECK(entries.size() == 2);
    MHF_CHECK(entries.at("mapped_entry") == 0x08800100u);
    MHF_CHECK(!entries.at("unmapped_entry").has_value());
    MHF_CHECK(!entries.contains("entry_point")); // [functions] are not data

    const auto out = dir.path / "generated";
    fs::create_directories(out);
    const auto path = emit_region_data("test_program", entries, out);
    MHF_CHECK(path == out / "test_program_region_data.hpp");
    const auto text = read(path);
    MHF_CHECK(contains(text, "#pragma once"));
    MHF_CHECK(contains(text, "namespace mhf::psp::generated::test_program_region_data {"));
    MHF_CHECK(contains(text, "inline constexpr std::optional<std::uint32_t> mapped_entry = 0x08800100u;"));
    MHF_CHECK(contains(text, "inline constexpr std::optional<std::uint32_t> unmapped_entry = std::nullopt;"));

    // Same entries: the file is left as it is, so builds that include it are not redone.
    const auto written = fs::last_write_time(path);
    emit_region_data("test_program", entries, out);
    MHF_CHECK(fs::last_write_time(path) == written);

    bool refused = false;
    try {
        emit_region_data("test_program", {{"2nd-entry", 0x08800100u}}, out);
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    MHF_CHECK(refused);

    return mhf::test::finish("region_data");
}
