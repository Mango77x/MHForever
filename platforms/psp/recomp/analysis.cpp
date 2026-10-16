#include "analysis.hpp"

#include "instruction_emitter.hpp"
#include "psprecomp/decoder.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <deque>
#include <optional>

namespace mhf::psp::recomp {

using psprecomp::DecodedInstruction;
using psprecomp::OpcodeKind;

bool in_ranges(const std::vector<CodeRange> &ranges, std::uint32_t address) noexcept {
    return std::any_of(ranges.begin(), ranges.end(),
                       [address](const CodeRange &r) { return address >= r.start && address < r.end; });
}

namespace {

constexpr std::size_t kJumpTableWindow = 24; // instructions scanned before a `jr` for the table pattern
constexpr std::size_t kMaxJumpTableEntries = 1024;

bool is_conditional_branch(OpcodeKind kind) {
    switch (kind) {
    case OpcodeKind::Beq: case OpcodeKind::Bne: case OpcodeKind::Beql: case OpcodeKind::Bnel:
    case OpcodeKind::Blez: case OpcodeKind::Bgtz: case OpcodeKind::Blezl: case OpcodeKind::Bgtzl:
    case OpcodeKind::Bltz: case OpcodeKind::Bgez: case OpcodeKind::Bltzl: case OpcodeKind::Bgezl:
    case OpcodeKind::Bc1f: case OpcodeKind::Bc1t: case OpcodeKind::Bc1fl: case OpcodeKind::Bc1tl:
    case OpcodeKind::Bvf: case OpcodeKind::Bvt: case OpcodeKind::Bvfl: case OpcodeKind::Bvtl:
        return true;
    default:
        return false;
    }
}

} // namespace

bool is_unconditional_branch(const psprecomp::DecodedInstruction &d) {
    switch (d.kind) {
    case OpcodeKind::Beq: case OpcodeKind::Beql: return d.rs == d.rt;
    case OpcodeKind::Bgez: case OpcodeKind::Bgezl: case OpcodeKind::Blez: case OpcodeKind::Blezl: return d.rs == 0u;
    default: return false;
    }
}

namespace {

bool is_link_branch(OpcodeKind kind) {
    return kind == OpcodeKind::Bltzal || kind == OpcodeKind::Bgezal || kind == OpcodeKind::Bltzall ||
           kind == OpcodeKind::Bgezall;
}

std::uint32_t branch_target(const DecodedInstruction &d, std::uint32_t pc) {
    return pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
}

std::uint32_t jump_target(const DecodedInstruction &d, std::uint32_t pc) {
    return ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
}

// Register that an instruction writes (0 when none or not a GPR write we track).
std::uint32_t written_gpr(const DecodedInstruction &d) {
    switch (d.kind) {
    case OpcodeKind::Addiu: case OpcodeKind::Slti: case OpcodeKind::Sltiu: case OpcodeKind::Andi:
    case OpcodeKind::Ori: case OpcodeKind::Xori: case OpcodeKind::Lui: case OpcodeKind::Lw: case OpcodeKind::Lh:
    case OpcodeKind::Lhu: case OpcodeKind::Lb: case OpcodeKind::Lbu: case OpcodeKind::Lwl: case OpcodeKind::Lwr:
    case OpcodeKind::Ext: case OpcodeKind::Ins: case OpcodeKind::Mfc1: case OpcodeKind::Cfc1: case OpcodeKind::Mfv:
        return d.rt;
    case OpcodeKind::Jal: case OpcodeKind::Bltzal: case OpcodeKind::Bgezal: case OpcodeKind::Bltzall:
    case OpcodeKind::Bgezall:
        return 31u;
    case OpcodeKind::Jalr:
        return d.rd == 0u ? 31u : d.rd;
    default:
        break;
    }
    // R-type ALU writes rd.
    switch (d.kind) {
    case OpcodeKind::Add: case OpcodeKind::Addu: case OpcodeKind::Sub: case OpcodeKind::Subu: case OpcodeKind::And:
    case OpcodeKind::Or: case OpcodeKind::Xor: case OpcodeKind::Nor: case OpcodeKind::Slt: case OpcodeKind::Sltu:
    case OpcodeKind::Max: case OpcodeKind::Min: case OpcodeKind::Movz: case OpcodeKind::Movn: case OpcodeKind::Sll:
    case OpcodeKind::Srl: case OpcodeKind::Sra: case OpcodeKind::Rotr: case OpcodeKind::Sllv: case OpcodeKind::Srlv:
    case OpcodeKind::Srav: case OpcodeKind::Rotrv: case OpcodeKind::Clz: case OpcodeKind::Clo: case OpcodeKind::Seb:
    case OpcodeKind::Seh: case OpcodeKind::Bitrev: case OpcodeKind::Wsbh: case OpcodeKind::Wsbw: case OpcodeKind::Mfhi:
    case OpcodeKind::Mflo:
        return d.rd;
    default:
        return 0u;
    }
}

// Tiny forward constant tracker over a straight run of instructions.
struct RegisterValue {
    bool known{};
    std::uint32_t value{};
    bool indexed{};        // value = known constant + some runtime index
    bool table_load{};     // register holds a word loaded from [value]
};

class Explorer {
public:
    explicit Explorer(const ProgramInput &input) : input_(input) {}

    [[nodiscard]] bool is_code(std::uint32_t address) const {
        return (address & 3u) == 0u && in_ranges(input_.code, address);
    }
    [[nodiscard]] bool is_import(std::uint32_t address) const { return imports_.contains(address); }

    void set_imports(const std::map<std::uint32_t, std::size_t> &imports) { imports_ = imports; }

    [[nodiscard]] DecodedInstruction decode(std::uint32_t address) const {
        return psprecomp::decode_allegrex(input_.memory->load32(address));
    }

    // Recognizes `lui/addiu/addu/lw/jr` (and close variants) ending at `jump_pc` and reads the table.
    [[nodiscard]] std::optional<JumpTable> find_jump_table(std::uint32_t jump_pc, std::uint32_t function_entry) const {
        const auto jr = decode(jump_pc);
        std::uint32_t start = jump_pc;
        for (std::size_t i = 0; i < kJumpTableWindow && start > function_entry && is_code(start - 4u); ++i) {
            start -= 4u;
        }
        std::array<RegisterValue, 32> regs{};
        std::optional<std::uint32_t> bound;
        for (std::uint32_t pc = start; pc < jump_pc; pc += 4u) {
            const auto d = decode(pc);
            const auto &rs = regs[d.rs];
            const auto &rt = regs[d.rt];
            RegisterValue result{};
            std::uint32_t dest = written_gpr(d);
            switch (d.kind) {
            case OpcodeKind::Lui:
                result = {true, static_cast<std::uint32_t>(static_cast<std::uint16_t>(d.immediate)) << 16u, false, false};
                break;
            case OpcodeKind::Addiu:
                if (rs.known && !rs.table_load) {
                    result = {true, rs.value + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate)),
                              rs.indexed, false};
                }
                break;
            case OpcodeKind::Ori:
                if (rs.known && !rs.indexed && !rs.table_load) {
                    result = {true, rs.value | static_cast<std::uint16_t>(d.immediate), false, false};
                }
                break;
            case OpcodeKind::Addu: {
                const bool s_const = rs.known && !rs.indexed && !rs.table_load;
                const bool t_const = rt.known && !rt.indexed && !rt.table_load;
                if (s_const != t_const) {
                    result = {true, s_const ? rs.value : rt.value, true, false};
                }
                break;
            }
            case OpcodeKind::Lw:
                if (rs.known && !rs.table_load) {
                    result = {true, rs.value + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate)),
                              rs.indexed, true};
                }
                break;
            case OpcodeKind::Sltiu:
                bound = static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate));
                break;
            default:
                break;
            }
            if (dest != 0u) {
                regs[dest] = result;
            }
        }
        const auto &target = regs[jr.rs];
        if (!target.known || !target.table_load || !target.indexed) {
            return std::nullopt;
        }
        JumpTable table;
        table.jump_pc = jump_pc;
        table.table_address = target.value;
        const std::uint32_t limit = bound && *bound > 0u && *bound <= kMaxJumpTableEntries
                                        ? *bound
                                        : static_cast<std::uint32_t>(kMaxJumpTableEntries);
        for (std::uint32_t i = 0; i < limit; ++i) {
            const std::uint32_t entry_address = table.table_address + i * 4u;
            if (!input_.memory->contains(entry_address, 4u)) {
                break;
            }
            const std::uint32_t entry = input_.memory->load32(entry_address);
            if (!is_code(entry)) {
                break;
            }
            // Without a bound, stop at entries that leave the function's neighbourhood.
            if (!bound && (entry < function_entry || entry - function_entry > 0x40000u)) {
                break;
            }
            table.targets.push_back(entry);
        }
        if (table.targets.empty()) {
            return std::nullopt;
        }
        return table;
    }

    // Walks the body reachable from `entry`. `seeds` decides whether a `j` leaves the function (tail call).
    // Collects call targets and code-pointer constants into `discovered`.
    Function explore(std::uint32_t entry, const std::set<std::uint32_t> &roots, const std::set<std::uint32_t> &seeds,
                     std::set<std::uint32_t> &calls, std::set<std::uint32_t> &pointers) const {
        Function function;
        function.entry = entry;
        std::deque<std::uint32_t> work{entry};
        function.labels.insert(entry);
        for (const auto r : roots) {
            if (is_code(r)) {
                function.labels.insert(r);
                work.push_back(r);
            }
        }
        auto add_label = [&](std::uint32_t address) {
            if (!is_code(address)) {
                return;
            }
            function.labels.insert(address);
            if (!function.instructions.contains(address)) {
                work.push_back(address);
            }
        };
        auto add_call = [&](std::uint32_t address) {
            if (is_code(address) && !is_import(address)) {
                calls.insert(address);
            }
        };

        while (!work.empty()) {
            std::uint32_t pc = work.front();
            work.pop_front();
            while (is_code(pc) && !function.instructions.contains(pc)) {
                function.instructions.insert(pc);
                const auto d = decode(pc);
                if (!d.has_delay_slot()) {
                    if (ends_block(d)) {
                        break; // undecodable: the emitter reports it at runtime
                    }
                    pc += 4u;
                    continue;
                }
                function.instructions.insert(pc + 4u); // delay slot
                if (is_conditional_branch(d.kind)) {
                    add_label(branch_target(d, pc));
                    if (!is_unconditional_branch(d)) {
                        add_label(pc + 8u);
                    }
                } else if (is_link_branch(d.kind)) {
                    add_call(branch_target(d, pc));
                    add_label(pc + 8u);
                } else if (d.kind == OpcodeKind::J) {
                    const std::uint32_t target = jump_target(d, pc);
                    if (!seeds.contains(target) || target == entry) {
                        add_label(target);
                    }
                } else if (d.kind == OpcodeKind::Jal) {
                    add_call(jump_target(d, pc));
                    add_label(pc + 8u);
                } else if (d.kind == OpcodeKind::Jalr) {
                    add_label(pc + 8u);
                } else if (d.kind == OpcodeKind::Jr && d.rs != 31u) {
                    if (auto table = find_jump_table(pc, entry)) {
                        for (const auto t : table->targets) {
                            add_label(t);
                        }
                        function.jump_tables.emplace(pc, std::move(*table));
                    } else {
                        function.has_unresolved_jump = true;
                    }
                }
                break; // every control transfer ends the linear run
            }
        }
        // Code addresses built with lui + addiu/ori. Scanned in address order over the whole body, delay slots
        // included, because compilers often put one half of the pair in a delay slot.
        std::array<std::optional<std::uint32_t>, 32> lui{};
        for (const auto address : function.instructions) {
            const auto d = decode(address);
            if (d.kind == OpcodeKind::Lui) {
                lui[d.rt] = static_cast<std::uint32_t>(static_cast<std::uint16_t>(d.immediate)) << 16u;
                continue;
            }
            if ((d.kind == OpcodeKind::Addiu || d.kind == OpcodeKind::Ori) && lui[d.rs]) {
                const std::uint32_t value = d.kind == OpcodeKind::Addiu
                    ? *lui[d.rs] + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate))
                    : *lui[d.rs] | static_cast<std::uint16_t>(d.immediate);
                if (is_code(value) && !is_import(value)) {
                    pointers.insert(value);
                }
            }
            if (const auto w = written_gpr(d); w != 0u && d.kind != OpcodeKind::Addiu && d.kind != OpcodeKind::Ori) {
                lui[w].reset();
            }
        }
        return function;
    }

private:
    const ProgramInput &input_;
    std::map<std::uint32_t, std::size_t> imports_;
};

} // namespace

Analysis analyze(const ProgramInput &input) {
    Analysis result;
    for (std::size_t i = 0; i < input.imports.size(); ++i) {
        result.import_by_stub.emplace(input.imports[i].address, i);
    }
    Explorer explorer{input};
    explorer.set_imports(result.import_by_stub);

    // Every aligned data word that points into code, with the address of the word.
    std::map<std::uint32_t, std::uint32_t> data_pointers; // word address -> value
    for (const auto &range : input.data) {
        for (std::uint32_t a = (range.start + 3u) & ~3u; a + 4u <= range.end; a += 4u) {
            if (input.memory->contains(a, 4u)) {
                const std::uint32_t value = input.memory->load32(a);
                if (explorer.is_code(value) && !explorer.is_import(value)) {
                    data_pointers.emplace(a, value);
                }
            }
        }
    }

    // Seeds are function entries. Call targets always become seeds. Code pointers (data words, lui/addiu constants)
    // become seeds only when they do not land inside a known body. Interior pointers are labels of the function that
    // contains them; for functions with a computed jump we could not decode, runs of data pointers into the function's
    // address extent are taken as its jump tables and their targets become extra roots of the body.
    std::set<std::uint32_t> seeds;
    std::map<std::uint32_t, Function> bodies;
    std::map<std::uint32_t, std::set<std::uint32_t>> roots; // extra roots per function
    std::set<std::uint32_t> covered;
    std::set<std::uint32_t> table_words;
    std::set<std::uint32_t> constant_pointers;
    std::deque<std::uint32_t> pending;
    auto add_seed = [&](std::uint32_t entry) {
        if (explorer.is_code(entry) && seeds.insert(entry).second) {
            pending.push_back(entry);
        }
    };
    auto explore_one = [&](std::uint32_t entry) {
        std::set<std::uint32_t> calls, pointers;
        Function f = explorer.explore(entry, roots[entry], seeds, calls, pointers);
        covered.insert(f.instructions.begin(), f.instructions.end());
        for (const auto &[pc, table] : f.jump_tables) {
            for (std::size_t i = 0; i < table.targets.size(); ++i) {
                table_words.insert(table.table_address + static_cast<std::uint32_t>(i) * 4u);
            }
        }
        for (const auto c : calls) {
            add_seed(c);
        }
        constant_pointers.insert(pointers.begin(), pointers.end());
        bodies[entry] = std::move(f);
    };
    auto explore_pending = [&]() {
        while (!pending.empty()) {
            const std::uint32_t entry = pending.front();
            pending.pop_front();
            explore_one(entry);
        }
    };
    auto extent_end = [&](std::uint32_t entry) {
        const auto next = seeds.upper_bound(entry);
        std::uint32_t end = next != seeds.end() ? *next : entry;
        for (const auto &r : input.code) {
            if (entry >= r.start && entry < r.end) {
                end = next != seeds.end() && *next < r.end ? *next : r.end;
            }
        }
        return end;
    };
    // Finds runs of >= 3 consecutive data pointers into [entry, end) not already known as table words.
    auto guess_tables = [&](std::uint32_t entry) {
        const std::uint32_t end = extent_end(entry);
        std::set<std::uint32_t> targets;
        for (auto it = data_pointers.begin(); it != data_pointers.end();) {
            auto run_end = it;
            std::size_t run = 0;
            while (run_end != data_pointers.end() && run_end->first == it->first + run * 4u &&
                   run_end->second >= entry && run_end->second < end && !table_words.contains(run_end->first)) {
                ++run_end;
                ++run;
            }
            if (run >= 3) {
                for (auto w = it; w != run_end; ++w) {
                    table_words.insert(w->first);
                    targets.insert(w->second);
                }
                it = run_end;
            } else {
                ++it;
            }
        }
        return targets;
    };

    for (const auto e : input.extra_entries) {
        add_seed(e);
    }
    std::size_t data_seeds = 0, constant_seeds = 0;
    std::set<std::uint32_t> guessed;
    for (;;) {
        explore_pending();
        // Functions with undecoded computed jumps: absorb their likely tables.
        bool grew = false;
        for (const auto &[entry, f] : bodies) {
            if (f.has_unresolved_jump && guessed.insert(entry).second) {
                const auto targets = guess_tables(entry);
                if (!targets.empty()) {
                    roots[entry].insert(targets.begin(), targets.end());
                    pending.push_back(entry);
                    grew = true;
                }
            }
        }
        if (grew) {
            continue;
        }
        // Promote the lowest code pointer that is neither a seed nor inside a known body. Constants built by code
        // (callbacks passed to the kernel, function pointers) go first: they are almost always real entries, and
        // exploring them first lets their bodies absorb the data pointers that are really jump-table targets.
        std::optional<std::uint32_t> next;
        bool from_constant = false;
        for (const auto value : constant_pointers) {
            if (!seeds.contains(value) && !covered.contains(value)) {
                next = value;
                from_constant = true;
                break;
            }
        }
        if (!next) {
            for (const auto &[word, value] : data_pointers) {
                if (!table_words.contains(word) && !seeds.contains(value) && !covered.contains(value) &&
                    (!next || value < *next)) {
                    next = value;
                }
            }
        }
        if (!next) {
            break;
        }
        (from_constant ? constant_seeds : data_seeds) += 1;
        if (std::getenv("MHF_RECOMP_VERBOSE") != nullptr) {
            std::fprintf(stderr, "promoted %08X (%s)\n", *next, from_constant ? "constant" : "data");
        }
        add_seed(*next);
    }

    // Final bodies with the final seed set (so `j` to another function becomes a tail call). Interior code pointers
    // become labels so unresolved computed jumps can reach them.
    std::set<std::uint32_t> interior;
    for (const auto &[word, value] : data_pointers) {
        if (!seeds.contains(value)) {
            interior.insert(value);
        }
    }
    for (const auto value : constant_pointers) {
        if (!seeds.contains(value)) {
            interior.insert(value);
        }
    }
    for (const auto entry : seeds) {
        std::set<std::uint32_t> calls, pointers;
        Function f = explorer.explore(entry, roots[entry], seeds, calls, pointers);
        for (const auto p : interior) {
            if (f.instructions.contains(p)) {
                f.labels.insert(p);
            }
        }
        result.jump_tables += f.jump_tables.size();
        result.unresolved_jumps += f.has_unresolved_jump ? 1u : 0u;
        result.functions.emplace(entry, std::move(f));
    }
    result.data_pointer_seeds = data_seeds;
    result.constant_pointer_seeds = constant_seeds;
    return result;
}

} // namespace mhf::psp::recomp
