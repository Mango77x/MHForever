// Region data (recomp/emitter.*): the entries of the active region map with the names only other region maps list,
// the header written from them (address or std::nullopt), the file left as it is when nothing changed, and a name that
// is not a C++ identifier refused; the [patches] entries read or refused, and which instructions each patch kind fits.
// Everything is written in a temporary folder from synthetic maps.

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

    // Instruction patches ([patches]): both kinds read, with their module and address; an entry that cannot be read is
    // refused; each kind only fits the instructions it can change.
    using mhf::psp::recomp::InstructionPatch;
    using mhf::psp::recomp::patch_mismatch;
    using mhf::psp::recomp::region_patches;
    const auto patched = dir.path / "regions" / "patched_region" / "symbols.toml";
    write(patched, "[patches]\nclear_flag_test = \"lobby_task.ovl 0x09AA2F3C immediate 0\"\n"
                   "skip_branch = \"executable 0x08804010 never\" # comment\n");
    const auto patches = region_patches(patched);
    MHF_CHECK(patches.size() == 2);
    for (const auto &p : patches) {
        if (p.patch.name == "clear_flag_test") {
            MHF_CHECK(p.module == "lobby_task.ovl" && p.address == 0x09AA2F3Cu);
            MHF_CHECK(p.patch.kind == InstructionPatch::Kind::Immediate && p.patch.immediate == 0);
        } else {
            MHF_CHECK(p.patch.name == "skip_branch" && p.module == "executable" && p.address == 0x08804010u);
            MHF_CHECK(p.patch.kind == InstructionPatch::Kind::Never);
        }
    }
    for (const char *bad : {"\"lobby_task.ovl 0x09AA2F3C immediate\"", "\"lobby_task.ovl 0x09AA2F3C sometimes\"",
                            "\"lobby_task.ovl 0x09AA2F3C immediate 0x10000\"", "\"0x09AA2F3C never\""}) {
        write(patched, std::string("[patches]\nbad = ") + bad + "\n");
        bool rejected = false;
        try {
            (void)region_patches(patched);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        MHF_CHECK(rejected);
    }
    InstructionPatch immediate;
    immediate.kind = InstructionPatch::Kind::Immediate;
    InstructionPatch never;
    never.kind = InstructionPatch::Kind::Never;
    const auto andi = psprecomp::decode_allegrex(0x30420020u);  // andi v0, v0, 0x20
    const auto bne = psprecomp::decode_allegrex(0x1440015Fu);   // bne v0, zero, +0x15F
    const auto b = psprecomp::decode_allegrex(0x10000004u);     // beq zero, zero (always taken)
    const auto lw = psprecomp::decode_allegrex(0x8E62004Cu);    // lw v0, 0x4C(s3)
    MHF_CHECK(patch_mismatch(andi, immediate).empty());
    MHF_CHECK(!patch_mismatch(lw, immediate).empty());
    MHF_CHECK(!patch_mismatch(bne, immediate).empty());
    MHF_CHECK(patch_mismatch(bne, never).empty());
    MHF_CHECK(!patch_mismatch(b, never).empty());
    MHF_CHECK(!patch_mismatch(andi, never).empty());

    return mhf::test::finish("region_data");
}
