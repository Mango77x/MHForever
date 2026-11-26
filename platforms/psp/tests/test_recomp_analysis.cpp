// Synthetic Allegrex instructions only; no game executable or region map needed.

#include "analysis.hpp"
#include "check.hpp"

#include <cstdint>
#include <initializer_list>

namespace {

constexpr std::uint32_t base = psprecomp::GuestMemory::kPhysicalBase;
constexpr std::uint32_t target = base + 0x100u;
// Far enough that an immediate with bit 15 set (0x8000) lands on it.
constexpr std::uint32_t far_target = base + 0x8000u;
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

mhf::psp::recomp::Analysis discover(std::initializer_list<std::uint32_t> words,
                                    std::uint32_t pointee) {
    psprecomp::GuestMemory memory(32u * 1024u * 1024u); // the constructor only accepts 32 or 64 MiB
    std::uint32_t pc = base;
    for (const auto word : words) {
        memory.store32(pc, word);
        pc += 4u;
    }
    memory.store32(pointee, jr_ra);
    memory.store32(pointee + 4u, 0u);

    mhf::psp::recomp::ProgramInput input;
    input.memory = &memory;
    input.code = {{base, pointee + 8u}};
    input.extra_entries = {base};
    return mhf::psp::recomp::analyze(input);
}

void expect_pointer(std::initializer_list<std::uint32_t> words, bool expected,
                    std::uint32_t pointee = target) {
    const auto result = discover(words, pointee);
    MHF_CHECK(result.functions.contains(base));
    MHF_CHECK(result.functions.contains(pointee) == expected);
    MHF_CHECK(result.constant_pointer_seeds == (expected ? 1u : 0u));
}

} // namespace

int main() {
    // Both normal LUI pairs and a pair completed in a delay slot still work.
    expect_pointer({lui(8u, 0x0800u), ori(9u, 8u, 0x100u), jr_ra, 0u}, true);
    expect_pointer({lui(8u, 0x0800u), addiu(8u, 8u, 0x100u), jr_ra, 0u}, true);
    expect_pointer({lui(8u, 0x0800u), jr_ra, addiu(9u, 8u, 0x100u)}, true);
    // Completing a pair into another register does not overwrite the LUI source.
    expect_pointer({lui(8u, 0x0800u), ori(9u, 8u, 0x8000u),
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

    // ADDIU sign-extends its immediate: 0x08010000 + 0xFFFF8000 = 0x08008000
    // (zero extension would give 0x08018000).
    expect_pointer({lui(8u, 0x0801u), addiu(9u, 8u, 0x8000u), jr_ra, 0u}, true, far_target);
    // ORI zero-extends its immediate: 0x08000000 | 0x00008000 = 0x08008000
    // (sign extension would give 0xFFFF8000).
    expect_pointer({lui(8u, 0x0800u), ori(9u, 8u, 0x8000u), jr_ra, 0u}, true, far_target);

    return mhf::test::finish("recomp_analysis");
}
