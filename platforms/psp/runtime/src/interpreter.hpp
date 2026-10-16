#pragma once

#include "mhf/psp/runtime.hpp"

#include <cstdint>

namespace psprecomp {
struct DecodedInstruction;
class GuestMemory;
} // namespace psprecomp

namespace mhf::psp {

// Runs the guest function at `address` with the interpreter until it returns to its caller ($ra at entry).
void interpret_call(Runtime &rt, Context &ctx, std::uint32_t address);

// Executes one VFPU (or unclassified Allegrex vector) instruction; false if it is not implemented.
// `rs_value` is the GPR base register value for the unaligned quad loads/stores.
bool interpret_vfpu(Context &ctx, psprecomp::GuestMemory &mem, const psprecomp::DecodedInstruction &d,
                    std::uint32_t rs_value);

} // namespace mhf::psp
