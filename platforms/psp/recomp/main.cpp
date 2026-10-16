// mhf_recomp — function-level Allegrex → C++ recompiler (docs/04-recompiler.md).
//
// Input: a decrypted PSP ELF (our own homebrew, or the developer's own game executable). Output: C++ sources in a
// git-ignored folder. Generated code from a game must never be committed (see CONTRIBUTING.md).

#include "analysis.hpp"
#include "emitter.hpp"
#include "mini_toml.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/nid_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mhf::psp::recomp;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: mhf_recomp --elf <file.elf> --out <dir> --name <identifier> [--parts N] [--nids nids.csv]\n"
                 "                  [--overlays overlays.toml] [--symbols symbols.toml --hook <name>...]\n"
                 "                  [--list-imports]\n");
}

constexpr std::uint32_t kShfAlloc = 0x2u;
constexpr std::uint32_t kShfExec = 0x4u;
constexpr std::uint32_t kShtNobits = 8u;

struct OverlayModule {
    std::string file_name;
    std::string identifier;
    std::uint32_t load_address{};
    std::int32_t overlay_id{};
    std::vector<std::uint8_t> bytes;
    ProgramInput input;
};

std::string identifier_for(const std::string &prefix, const std::string &file_name) {
    std::string stem = fs::path(file_name).stem().string();
    for (char &c : stem) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return prefix + "_ovl_" + stem;
}

// Reads the manifest written by mhfu_extract_overlays and the overlay files next to it. Layout of each file:
// 64-byte "MWo3" header, then .text (text_size bytes), then .data (data_size bytes), loaded verbatim at load_address.
std::vector<OverlayModule> load_overlays(const fs::path &manifest, const std::string &prefix) {
    const auto doc = toml::parse_file(manifest);
    std::vector<OverlayModule> result;
    const auto it = doc.table_arrays.find("overlay");
    if (it == doc.table_arrays.end()) {
        return result;
    }
    for (const auto &entry : it->second) {
        OverlayModule o;
        o.file_name = toml::get(entry, "name").value_or("");
        o.identifier = identifier_for(prefix, o.file_name);
        o.load_address = static_cast<std::uint32_t>(toml::to_integer(toml::get(entry, "load_address").value_or("0")));
        const auto text = static_cast<std::uint32_t>(toml::to_integer(toml::get(entry, "text_size").value_or("0")));
        const auto data = static_cast<std::uint32_t>(toml::to_integer(toml::get(entry, "data_size").value_or("0")));
        std::ifstream in(manifest.parent_path() / o.file_name, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open overlay " + o.file_name);
        }
        o.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (o.bytes.size() < 64 + static_cast<std::size_t>(text) + data || std::memcmp(o.bytes.data(), "MWo3", 4) != 0) {
            throw std::runtime_error("malformed overlay " + o.file_name);
        }
        std::memcpy(&o.overlay_id, o.bytes.data() + 4, 4);
        const std::uint32_t code_start = o.load_address + 64u;
        o.input.code.push_back({code_start, code_start + text});
        o.input.data.push_back({code_start + text, code_start + text + data});
        result.push_back(std::move(o));
    }
    return result;
}

struct Stats {
    std::size_t functions{}, instructions{}, covered{}, code_words{}, unresolved{};
    void add(const ProgramInput &input, const Analysis &analysis, const EmitResult &r) {
        functions += r.functions;
        instructions += r.instructions;
        unresolved += analysis.unresolved_jumps;
        std::set<std::uint32_t> c;
        for (const auto &[entry, f] : analysis.functions) c.insert(f.instructions.begin(), f.instructions.end());
        covered += c.size();
        for (const auto &range : input.code) code_words += (range.end - range.start) / 4u;
    }
};

void report(const std::string &name, const ProgramInput &input, const Analysis &analysis, const EmitResult &result) {
    Stats s;
    s.add(input, analysis, result);
    std::printf("%s: %zu functions, %zu instructions, %zu imports, %zu jump tables, %zu unresolved jumps, "
                "%zu data-pointer seeds, %zu constant-pointer seeds, coverage %.1f%%\n",
                name.c_str(), result.functions, result.instructions, input.imports.size(), analysis.jump_tables,
                analysis.unresolved_jumps, analysis.data_pointer_seeds, analysis.constant_pointer_seeds,
                s.code_words != 0 ? 100.0 * static_cast<double>(s.covered) / static_cast<double>(s.code_words) : 0.0);
}

} // namespace

int main(int argc, char **argv) {
    fs::path elf_path, out_dir, nids_path, overlays_path, symbols_path;
    std::vector<std::string> hook_names;
    std::string name;
    std::size_t parts = 8;
    bool list_imports = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--elf") elf_path = value();
        else if (arg == "--out") out_dir = value();
        else if (arg == "--name") name = value();
        else if (arg == "--parts") parts = std::stoul(value());
        else if (arg == "--nids") nids_path = value();
        else if (arg == "--list-imports") list_imports = true;
        else if (arg == "--overlays") overlays_path = value();
        else if (arg == "--symbols") symbols_path = value();
        else if (arg == "--hook") hook_names.push_back(value());
        else {
            usage();
            return 2;
        }
    }
    if (elf_path.empty() || (!list_imports && (out_dir.empty() || name.empty()))) {
        usage();
        return 2;
    }

    try {
        const auto elf = psprecomp::Elf32Image::from_file(elf_path);
        if (elf.is_psp_prx()) {
            std::fprintf(stderr, "error: relocatable PRX modules are not supported yet; build a static ELF\n");
            return 1;
        }
        psprecomp::GuestMemory memory;
        elf.load_into(memory, 0u);
        const auto module = elf.find_module_info(memory, 0u);
        if (!module) {
            std::fprintf(stderr, "error: no sceModuleInfo found\n");
            return 1;
        }

        ProgramInput input;
        input.memory = &memory;
        for (const auto &s : elf.sections()) {
            if ((s.flags & kShfAlloc) == 0 || s.size == 0 || s.address == 0) {
                continue;
            }
            if ((s.flags & kShfExec) != 0) {
                if (s.name != ".sceStub.text") {
                    input.code.push_back({s.address, s.address + s.size});
                }
            } else if (s.type != kShtNobits) {
                input.data.push_back({s.address, s.address + s.size});
            }
        }
        for (const auto &imp : elf.scan_imports(memory, *module)) {
            input.imports.push_back({imp.library, imp.nid, imp.stub_address});
        }
        input.extra_entries.push_back(elf.entry());

        if (list_imports) {
            psprecomp::NidRegistry nids;
            if (!nids_path.empty()) {
                nids.load_csv(nids_path);
            }
            for (const auto &imp : input.imports) {
                const auto resolved = nids.resolve(imp.library, imp.nid);
                std::printf("%s %s %s\n", imp.library.c_str(), psprecomp::hex32(imp.nid).c_str(),
                            resolved ? resolved->c_str() : "?");
            }
            return 0;
        }

        // Hooks by canonical name (games/<id>/regions/<id>/symbols.toml).
        if (!hook_names.empty()) {
            if (symbols_path.empty()) {
                std::fprintf(stderr, "error: --hook needs --symbols\n");
                return 2;
            }
            const auto symbols = toml::parse_file(symbols_path);
            const auto functions = symbols.tables.find("functions");
            for (const auto &hook : hook_names) {
                const auto value = functions != symbols.tables.end() ? toml::get(functions->second, hook) : std::nullopt;
                if (!value) {
                    std::fprintf(stderr, "error: symbol %s not found in %s\n", hook.c_str(),
                                 symbols_path.string().c_str());
                    return 1;
                }
                input.hooks[static_cast<std::uint32_t>(toml::to_integer(*value))] = hook;
            }
        }

        // Overlays (docs/04-recompiler.md §4): analyzed together with the main module so that calls between them
        // seed each other.
        std::vector<OverlayModule> overlays;
        if (!overlays_path.empty()) {
            overlays = load_overlays(overlays_path, name);
        }
        for (auto &o : overlays) {
            o.input.memory = &memory;
            o.input.imports = {};
            o.input.hooks = input.hooks;
        }
        auto route = [&](std::uint32_t target, bool from_main) {
            if (in_ranges(input.code, target)) {
                if (!from_main) input.extra_entries.push_back(target);
                return;
            }
            for (auto &o : overlays) {
                if (in_ranges(o.input.code, target)) o.input.weak_entries.push_back(target);
            }
        };
        for (auto &o : overlays) { // round 1
            memory.copy_in(o.load_address, o.bytes);
            for (const auto t : analyze(o.input).external_calls) route(t, false);
        }
        const auto analysis = analyze(input);
        for (const auto t : analysis.external_calls) route(t, true);

        EmitOptions options;
        options.program_name = name;
        options.entry = elf.entry();
        options.gp = module->gp;
        options.parts = parts;
        options.code_start = input.code.empty() ? 0u : input.code.front().start;
        options.code_end = input.code.empty() ? 0u : input.code.back().end;
        const auto result = emit_program(input, analysis, options, out_dir);
        report(name, input, analysis, result);

        std::vector<std::string> names{name};
        Stats totals;
        for (auto &o : overlays) { // round 2: final analysis and emission
            memory.copy_in(o.load_address, o.bytes);
            const auto overlay_analysis = analyze(o.input);
            EmitOptions overlay_options;
            overlay_options.program_name = o.identifier;
            overlay_options.gp = module->gp;
            std::size_t overlay_instructions = 0;
            for (const auto &[entry, f] : overlay_analysis.functions) overlay_instructions += f.instructions.size();
            overlay_options.parts = std::max<std::size_t>(1, overlay_instructions / 8000);
            overlay_options.code_start = o.input.code.front().start;
            overlay_options.code_end = o.input.code.front().end;
            overlay_options.overlay_id = o.overlay_id;
            const auto r = emit_program(o.input, overlay_analysis, overlay_options, out_dir);
            totals.add(o.input, overlay_analysis, r);
            names.push_back(o.identifier);
        }
        if (!overlays.empty()) {
            emit_registry(name, names, out_dir);
            std::printf("overlays: %zu modules, %zu functions, %zu instructions, coverage %.1f%%, %zu unresolved jumps\n",
                        overlays.size(), totals.functions, totals.instructions,
                        totals.code_words != 0 ? 100.0 * static_cast<double>(totals.covered) /
                                                     static_cast<double>(totals.code_words)
                                               : 0.0,
                        totals.unresolved);
        } else {
            emit_registry(name, names, out_dir);
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
