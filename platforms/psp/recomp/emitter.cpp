#include "emitter.hpp"

#include "instruction_emitter.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"

#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace mhf::psp::recomp {

using psprecomp::DecodedInstruction;
using psprecomp::OpcodeKind;
using psprecomp::hex32;

namespace {

std::string hex_digits(std::uint32_t v) { return hex32(v).substr(2); }
std::string label(std::uint32_t pc) { return "L_" + hex_digits(pc); }
std::string fn_name(std::uint32_t entry) { return "f_" + hex_digits(entry); }

bool is_likely(OpcodeKind kind) {
    return kind == OpcodeKind::Beql || kind == OpcodeKind::Bnel || kind == OpcodeKind::Blezl ||
           kind == OpcodeKind::Bgtzl || kind == OpcodeKind::Bltzl || kind == OpcodeKind::Bgezl ||
           kind == OpcodeKind::Bltzall || kind == OpcodeKind::Bgezall || kind == OpcodeKind::Bc1fl ||
           kind == OpcodeKind::Bc1tl || kind == OpcodeKind::Bvfl || kind == OpcodeKind::Bvtl;
}

bool is_link_branch(OpcodeKind kind) {
    return kind == OpcodeKind::Bltzal || kind == OpcodeKind::Bgezal || kind == OpcodeKind::Bltzall ||
           kind == OpcodeKind::Bgezall;
}

// Branch conditions. Derived from PSPRecomp's codegen_main.cpp (MIT, see third_party/psprecomp/LICENSE).
std::string branch_condition(const DecodedInstruction &d) {
    switch (d.kind) {
    case OpcodeKind::Beq: case OpcodeKind::Beql: return reg(d.rs) + " == " + reg(d.rt);
    case OpcodeKind::Bne: case OpcodeKind::Bnel: return reg(d.rs) + " != " + reg(d.rt);
    case OpcodeKind::Blez: case OpcodeKind::Blezl: return "static_cast<std::int32_t>(" + reg(d.rs) + ") <= 0";
    case OpcodeKind::Bgtz: case OpcodeKind::Bgtzl: return "static_cast<std::int32_t>(" + reg(d.rs) + ") > 0";
    case OpcodeKind::Bltz: case OpcodeKind::Bltzl: case OpcodeKind::Bltzal: case OpcodeKind::Bltzall:
        return "static_cast<std::int32_t>(" + reg(d.rs) + ") < 0";
    case OpcodeKind::Bgez: case OpcodeKind::Bgezl: case OpcodeKind::Bgezal: case OpcodeKind::Bgezall:
        return "static_cast<std::int32_t>(" + reg(d.rs) + ") >= 0";
    case OpcodeKind::Bc1f: case OpcodeKind::Bc1fl: return "!ctx.fpu_condition()";
    case OpcodeKind::Bc1t: case OpcodeKind::Bc1tl: return "ctx.fpu_condition()";
    case OpcodeKind::Bvf: case OpcodeKind::Bvfl:
        return "((ctx.vfpu_ctrl[3] >> " + std::to_string((d.word >> 18u) & 7u) + "u) & 1u) == 0u";
    case OpcodeKind::Bvt: case OpcodeKind::Bvtl:
        return "((ctx.vfpu_ctrl[3] >> " + std::to_string((d.word >> 18u) & 7u) + "u) & 1u) != 0u";
    default:
        throw std::logic_error("branch_condition: not a conditional branch");
    }
}

class FunctionWriter {
public:
    FunctionWriter(const ProgramInput &input, const Analysis &analysis, const Function &function)
        : input_(input), analysis_(analysis), function_(function) {}

    std::string write() {
        out_ << "void " << fn_name(function_.entry) << "(Runtime &rt, Context &ctx) {\n"
             << "    psprecomp::GuestMemory &mem = rt.memory();\n"
             << "    std::uint32_t jump_target = 0u;\n"
             << "    (void)mem; (void)jump_target;\n";
        if (const auto hook = input_.hooks.find(function_.entry); hook != input_.hooks.end()) {
            out_ << "    { static const HookFunction hook = rt.find_hook(\"" << cpp_escape(hook->second) << "\");\n"
                 << "      if (hook != nullptr) hook(rt, ctx); }\n";
        }
        // The entry block first, then the others in address order.
        write_block(function_.entry);
        for (const auto l : function_.labels) {
            if (l != function_.entry) {
                write_block(l);
            }
        }
        out_ << "RETURN_MISMATCH:\n";
        if (!continuations_.empty()) {
            out_ << "    switch (ctx.gpr[31]) {\n";
            for (const auto c : continuations_) {
                out_ << "    case " << hex32(c) << "u: goto " << label(c) << ";\n";
            }
            out_ << "    default: break;\n    }\n";
        }
        out_ << "    return;\n}\n\n";
        return out_.str();
    }

private:
    [[nodiscard]] DecodedInstruction decode(std::uint32_t pc) const {
        return psprecomp::decode_allegrex(input_.memory->load32(pc));
    }

    // Transfer that leaves the current function at `target` (tail position: the caller returns right after).
    [[nodiscard]] std::string call_expression(std::uint32_t target) const {
        if (const auto it = analysis_.import_by_stub.find(target); it != analysis_.import_by_stub.end()) {
            return "call_import(rt, ctx, kImports[" + std::to_string(it->second) + "])";
        }
        if (analysis_.functions.contains(target)) {
            return fn_name(target) + "(rt, ctx)";
        }
        return "rt.call(ctx, " + hex32(target) + "u)";
    }

    // A callee normally returns with the link register still holding the address after the call. Anything else is
    // a non-local return (longjmp): resume at the matching call continuation of this frame, or leave this frame too.
    void return_check(std::uint32_t link, std::uint32_t after) {
        out_ << "    if (ctx.gpr[" << link << "] != " << hex32(after) << "u) goto RETURN_MISMATCH;\n";
        if (function_.labels.contains(after)) {
            continuations_.insert(after);
        }
    }

    // Continue at `target` inside this function if it is a block of ours, otherwise tail-call it.
    void goto_or_tail(std::uint32_t target, const char *indent) {
        if (function_.labels.contains(target)) {
            out_ << indent << "goto " << label(target) << ";\n";
        } else {
            out_ << indent << call_expression(target) << "; return;\n";
        }
    }

    void write_block(std::uint32_t start) {
        out_ << label(start) << ":\n";
        std::uint32_t pc = start;
        for (;;) {
            if (pc != start && function_.labels.contains(pc)) {
                out_ << "    goto " << label(pc) << ";\n";
                return;
            }
            if (!function_.instructions.contains(pc)) {
                // Fell off the analyzed body (end of code, or undecodable instruction before).
                out_ << "    rt.jump(ctx, " << hex32(pc) << "u); return;\n";
                return;
            }
            const auto d = decode(pc);
            if (!d.has_delay_slot() && d.kind != OpcodeKind::Jr && d.kind != OpcodeKind::Jalr) {
                out_ << emit_instruction(d, pc);
                if (ends_block(d)) {
                    return; // emit_instruction already reported and returned
                }
                ++instruction_count_;
                pc += 4u;
                continue;
            }
            write_control_transfer(d, pc);
            return;
        }
    }

    void write_control_transfer(const DecodedInstruction &d, std::uint32_t pc) {
        const auto slot = decode(pc + 4u);
        instruction_count_ += 2;
        if (slot.is_control_flow() || slot.kind == OpcodeKind::Jr || slot.kind == OpcodeKind::Jalr) {
            out_ << "    rt.unsupported(" << hex32(pc + 4u) << "u, " << hex32(slot.word)
                 << "u, \"control transfer in a delay slot\"); return;\n";
            return;
        }
        const std::string slot_code = emit_instruction(slot, pc + 4u);
        const std::uint32_t after = pc + 8u;

        if (is_link_branch(d.kind)) {
            const std::uint32_t target = pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
            out_ << "    { const bool taken = " << branch_condition(d) << ";\n"
                 << "    ctx.set_gpr(31, " << hex32(after) << "u);\n";
            if (is_likely(d.kind)) {
                out_ << "    if (taken) {\n" << slot_code << "    " << call_expression(target) << ";\n    }\n";
            } else {
                out_ << slot_code << "    if (taken) " << call_expression(target) << ";\n";
            }
            out_ << "    }\n";
            return_check(31u, after);
            goto_or_tail(after, "    ");
            return;
        }

        switch (d.kind) {
        case OpcodeKind::J: {
            const std::uint32_t target = ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
            out_ << slot_code;
            goto_or_tail(target, "    ");
            return;
        }
        case OpcodeKind::Jal: {
            const std::uint32_t target = ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
            out_ << "    ctx.set_gpr(31, " << hex32(after) << "u);\n" << slot_code
                 << "    " << call_expression(target) << ";\n";
            return_check(31u, after);
            goto_or_tail(after, "    ");
            return;
        }
        case OpcodeKind::Jalr: {
            const std::uint32_t link = d.rd == 0u ? 31u : d.rd;
            out_ << "    jump_target = " << reg(d.rs) << ";\n"
                 << "    ctx.set_gpr(" << link << ", " << hex32(after) << "u);\n" << slot_code
                 << "    rt.call(ctx, jump_target);\n";
            return_check(link, after);
            goto_or_tail(after, "    ");
            return;
        }
        case OpcodeKind::Jr: {
            if (d.rs == 31u) {
                out_ << slot_code << "    return;\n";
                return;
            }
            out_ << "    jump_target = " << reg(d.rs) << ";\n" << slot_code;
            std::set<std::uint32_t> cases;
            if (const auto it = function_.jump_tables.find(pc); it != function_.jump_tables.end()) {
                cases.insert(it->second.targets.begin(), it->second.targets.end());
            } else {
                cases = function_.labels; // unknown computed jump: any block of ours, else leave
            }
            out_ << "    switch (jump_target) {\n";
            for (const auto c : cases) {
                out_ << "    case " << hex32(c) << "u: goto " << label(c) << ";\n";
            }
            out_ << "    default: break;\n    }\n"
                 << "    rt.jump(ctx, jump_target); return;\n";
            return;
        }
        default:
            break;
        }

        // Conditional branches.
        const std::uint32_t target = pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
        if (is_unconditional_branch(d)) {
            out_ << slot_code;
            goto_or_tail(target, "    ");
            return;
        }
        if (is_likely(d.kind)) {
            out_ << "    if (" << branch_condition(d) << ") {\n" << slot_code;
            goto_or_tail(target, "        ");
            out_ << "    }\n";
        } else {
            out_ << "    { const bool taken = " << branch_condition(d) << ";\n" << slot_code
                 << "    if (taken) {\n";
            goto_or_tail(target, "        ");
            out_ << "    } }\n";
        }
        goto_or_tail(after, "    ");
    }

public:
    std::size_t instruction_count_{};

private:
    const ProgramInput &input_;
    const Analysis &analysis_;
    const Function &function_;
    std::ostringstream out_;
    std::set<std::uint32_t> continuations_;
};

void write_if_changed(const std::filesystem::path &path, const std::string &text) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            const std::string current{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            if (current == text) {
                return;
            }
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
    out << text;
}

} // namespace

EmitResult emit_program(const ProgramInput &input, const Analysis &analysis, const EmitOptions &options,
                        const std::filesystem::path &out_dir) {
    if (options.parts == 0) {
        throw std::invalid_argument("emit_program: parts must be at least 1");
    }
    std::filesystem::create_directories(out_dir);
    EmitResult result;
    const std::string ns = "mhf_gen::" + options.program_name;
    const std::string header_name = options.program_name + ".hpp";

    // Header with every declaration.
    std::ostringstream header;
    header << "// Generated by mhf_recomp. Do not edit and never commit (see CONTRIBUTING.md).\n"
           << "#pragma once\n#include \"mhf/psp/generated.hpp\"\n\n"
           << "namespace " << ns << " {\n"
           << "using namespace mhf::psp::generated;\n"
           << "extern ImportEntry kImports[];\n";
    for (const auto &[entry, f] : analysis.functions) {
        header << "void " << fn_name(entry) << "(Runtime &rt, Context &ctx);\n";
    }
    header << "} // namespace " << ns << "\n";
    write_if_changed(out_dir / header_name, header.str());
    result.files.push_back(out_dir / header_name);

    // Function bodies split into `parts` files of similar instruction counts.
    std::size_t total_instructions = 0;
    for (const auto &[entry, f] : analysis.functions) {
        total_instructions += f.instructions.size();
    }
    const std::size_t per_part = total_instructions / options.parts + 1;
    auto it = analysis.functions.begin();
    for (std::size_t part = 0; part < options.parts; ++part) {
        std::ostringstream src;
        src << "// Generated by mhf_recomp. Do not edit and never commit (see CONTRIBUTING.md).\n"
            << "#include \"" << header_name << "\"\n\n"
            << "namespace " << ns << " {\n\n";
        std::size_t in_part = 0;
        while (it != analysis.functions.end() && (in_part < per_part || part + 1 == options.parts)) {
            FunctionWriter writer{input, analysis, it->second};
            src << writer.write();
            in_part += it->second.instructions.size();
            result.instructions += writer.instruction_count_;
            ++result.functions;
            ++it;
        }
        src << "} // namespace " << ns << "\n";
        const auto path = out_dir / (options.program_name + "_part" + std::to_string(part) + ".cpp");
        write_if_changed(path, src.str());
        result.files.push_back(path);
    }

    // Tables: functions (sorted by address), imports and the program description.
    std::ostringstream table;
    table << "// Generated by mhf_recomp. Do not edit and never commit (see CONTRIBUTING.md).\n"
          << "#include \"" << header_name << "\"\n\n"
          << "namespace " << ns << " {\n\n"
          << "ImportEntry kImports[] = {\n";
    for (const auto &imp : input.imports) {
        table << "    {\"" << cpp_escape(imp.library) << "\", " << hex32(imp.nid) << "u, " << hex32(imp.address)
              << "u, nullptr},\n";
    }
    if (input.imports.empty()) {
        table << "    {\"\", 0u, 0u, nullptr},\n";
    }
    table << "};\n\nnamespace {\n";
    for (std::size_t i = 0; i < input.imports.size(); ++i) {
        table << "void imp_" << i << "(Runtime &rt, Context &ctx) { call_import(rt, ctx, kImports[" << i
              << "]); }\n";
    }
    table << "\nconst FunctionEntry kFunctions[] = {\n";
    // Import stubs and functions in one address-sorted table.
    std::map<std::uint32_t, std::string> all;
    for (const auto &[entry, f] : analysis.functions) {
        all.emplace(entry, fn_name(entry));
    }
    for (std::size_t i = 0; i < input.imports.size(); ++i) {
        all.emplace(input.imports[i].address, "imp_" + std::to_string(i));
    }
    for (const auto &[address, name] : all) {
        table << "    {" << hex32(address) << "u, &" << name << "},\n";
    }
    if (all.empty()) {
        table << "    {0u, nullptr},\n"; // no functions found; the span below is empty
    }
    table << "};\n} // namespace\n\n"
          << "} // namespace " << ns << "\n\n"
          << "namespace mhf::psp::generated {\n"
          << "const Program &" << options.program_name << "_program() {\n"
          << "    static const Program program{\"" << cpp_escape(options.program_name) << "\", "
          << hex32(options.entry) << "u, " << hex32(options.gp) << "u,\n"
          << "        std::span<const FunctionEntry>(" << ns << "::kFunctions, " << all.size() << "u),\n"
          << "        std::span<ImportEntry>(" << ns << "::kImports, " << input.imports.size() << "u),\n"
          << "        " << hex32(options.code_start) << "u, " << hex32(options.code_end) << "u, "
          << options.overlay_id << "};\n"
          << "    return program;\n}\n"
          << "} // namespace mhf::psp::generated\n";
    const auto table_path = out_dir / (options.program_name + "_table.cpp");
    write_if_changed(table_path, table.str());
    result.files.push_back(table_path);
    return result;
}

} // namespace mhf::psp::recomp

namespace mhf::psp::recomp {

void emit_registry(const std::string &registry_name, const std::vector<std::string> &program_names,
                   const std::filesystem::path &out_dir) {
    std::ostringstream src;
    src << "// Generated by mhf_recomp. Do not edit and never commit (see CONTRIBUTING.md).\n"
        << "#include \"mhf/psp/generated.hpp\"\n\n"
        << "namespace mhf::psp::generated {\n";
    for (const auto &name : program_names) {
        src << "const Program &" << name << "_program();\n";
    }
    src << "\nstd::span<const Program *const> " << registry_name << "_programs() {\n"
        << "    static const Program *const programs[] = {\n";
    for (const auto &name : program_names) {
        src << "        &" << name << "_program(),\n";
    }
    src << "    };\n    return programs;\n}\n} // namespace mhf::psp::generated\n";
    write_if_changed(out_dir / (registry_name + "_programs.cpp"), src.str());
}

} // namespace mhf::psp::recomp
