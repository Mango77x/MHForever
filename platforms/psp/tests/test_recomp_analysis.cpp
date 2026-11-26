// Synthetic Allegrex instructions only; no game executable or region map needed.

#include "analysis.hpp"
#include "check.hpp"

#include <cstdint>
#include <initializer_list>

namespace {

constexpr std::uint32_t base = psprecomp::GuestMemory::kPhysicalBase;
constexpr std::uint32_t target = base + 0x100u;
constexpr std::uint32_t jr_ra = 0x03E00008u;

constexpr std::uint32_t lui(std::uint32_t rt, std::uint32_t imm) {
    return 0x3C000000u | (rt << 16u) | imm;
}

constexpr std::uint32_t ori(std::uint32_t rt, std::uint32_t rs, std::uint32_t imm) {
    return 0x34000000u | (rs << 21u) | (rt << 16u) | imm;
}

constexpr std::uint32_t addiu(std::uint32_t rt, std::uint32_t rs, std::uint32_t imm) {
    return 0x24000000u | (rs << 21u) | (rt << 16u) | imm;
}

mhf::psp::recomp::Analysis discover(std::initializer_list<std::uint32_t> words) {
    psprecomp::GuestMemory memory(4096u);
    std::uint32_t pc = base;
    for (const auto word : words) {
        memory.store32(pc, word);
        pc += 4u;
    }
    memory.store32(target, jr_ra);
    memory.store32(target + 4u, 0u);

    mhf::psp::recomp::ProgramInput input;
    input.memory = &memory;
    input.code = {{base, target + 8u}};
    input.extra_entries = {base};
    return mhf::psp::recomp::analyze(input);
}

void expect_pointer(std::initializer_list<std::uint32_t> words, bool expected) {
    const auto result = discover(words);
    MHF_CHECK(result.functions.contains(base));
    MHF_CHECK(result.functions.contains(target) == expected);
    MHF_CHECK(result.constant_pointer_seeds == (expected ? 1u : 0u));
}

} // namespace

int main() {
    // Both normal LUI pairs and a pair completed in a delay slot still work.
    expect_pointer({lui(8u, 0x0800u), ori(9u, 8u, 0x100u), jr_ra, 0u}, true);
    expect_pointer({lui(8u, 0x0800u), addiu(8u, 8u, 0x100u), jr_ra, 0u}, true);
    expect_pointer({lui(8u, 0x0800u), jr_ra, addiu(9u, 8u, 0x100u)}, true);
    // Completing a pair into another register does not overwrite the LUI source.
    expect_pointer({lui(8u, 0x0800u), ori(9u, 8u, 0x20u),
                    addiu(10u, 8u, 0x100u), jr_ra, 0u}, true);

    // ORI and ADDIU overwrite the tracked register even when their source is unknown.
    expect_pointer({lui(8u, 0x0800u), ori(8u, 4u, 0u),
                    ori(9u, 8u, 0x100u), jr_ra, 0u}, false);
    expect_pointer({lui(8u, 0x0800u), addiu(8u, 4u, 0u),
                    addiu(9u, 8u, 0x100u), jr_ra, 0u}, false);
    // An in-place ADDIU has changed the high half: the second pair is not valid.
    expect_pointer({lui(8u, 0x0800u), addiu(8u, 8u, 0xFFFFu),
                    ori(9u, 8u, 0x100u), jr_ra, 0u}, false);
    // LUI cannot change $zero.
    expect_pointer({lui(0u, 0x0800u), ori(9u, 0u, 0x100u), jr_ra, 0u}, false);

    return mhf::test::finish("recomp_analysis");
}
