// SysMemUserForUser: partition memory.

#include "hle.hpp"
#include "kernel.hpp"

#include <cstdio>

namespace mhf::psp::hle {

namespace {

SysMem &sysmem(Runtime &rt) { return rt.kernel().sysmem(); }

void MaxFreeMemSize(Runtime &rt, Context &ctx) { ret(ctx, sysmem(rt).max_free(2)); }
void TotalFreeMemSize(Runtime &rt, Context &ctx) { ret(ctx, sysmem(rt).total_free(2)); }

void AllocPartitionMemory(Runtime &rt, Context &ctx) {
    const int partition = sarg(ctx, 0);
    const std::string name = read_string(rt, arg(ctx, 1), 32);
    const std::uint32_t type = arg(ctx, 2);
    const std::uint32_t size = arg(ctx, 3);
    const std::uint32_t address = arg(ctx, 4);
    SysMem::Placement placement{};
    std::uint32_t extra = 0;
    switch (type) {
    case 0: placement = SysMem::Placement::Low; break;
    case 1: placement = SysMem::Placement::High; break;
    case 2: placement = SysMem::Placement::Address; extra = address; break;
    case 3: placement = SysMem::Placement::Low; extra = address; break;  // aligned low
    case 4: placement = SysMem::Placement::High; extra = address; break; // aligned high
    default: ret(ctx, 0x800200D8u); return;                             // ILLEGAL_MEMBLOCK_ALLOC_TYPE
    }
    const auto block = sysmem(rt).allocate(partition, name, placement, size, extra);
    ret(ctx, block ? static_cast<std::uint32_t>(block->uid) : 0x800200D9u); // FAILED_ALLOC_MEMBLOCK
}

void FreePartitionMemory(Runtime &rt, Context &ctx) {
    ret(ctx, sysmem(rt).free(sarg(ctx, 0)) ? 0u : 0x800200CBu);
}

void GetBlockHeadAddr(Runtime &rt, Context &ctx) {
    const auto *block = sysmem(rt).find(sarg(ctx, 0));
    ret(ctx, block != nullptr ? block->address : 0u);
}

void DevkitVersion(Runtime &, Context &ctx) { ret(ctx, 0x06060010u); }
void SetCompiledSdkVersion(Runtime &, Context &ctx) { ret(ctx, 0); }
void GetCompiledSdkVersion(Runtime &, Context &ctx) { ret(ctx, 0x06060010u); }

void Printf(Runtime &rt, Context &ctx) {
    std::fprintf(stderr, "[guest] %s", read_string(rt, arg(ctx, 0), 1024).c_str());
    ret(ctx, 0);
}

const HleFunction kFunctions[] = {
    {"SysMemUserForUser", 0xA291F107u, "sceKernelMaxFreeMemSize", &MaxFreeMemSize},
    {"SysMemUserForUser", 0xF919F628u, "sceKernelTotalFreeMemSize", &TotalFreeMemSize},
    {"SysMemUserForUser", 0x237DBD4Fu, "sceKernelAllocPartitionMemory", &AllocPartitionMemory},
    {"SysMemUserForUser", 0xB6D61D02u, "sceKernelFreePartitionMemory", &FreePartitionMemory},
    {"SysMemUserForUser", 0x9D9A5BA1u, "sceKernelGetBlockHeadAddr", &GetBlockHeadAddr},
    {"SysMemUserForUser", 0x3FC9AE6Au, "sceKernelDevkitVersion", &DevkitVersion},
    {"SysMemUserForUser", 0x7591C7DBu, "sceKernelSetCompiledSdkVersion", &SetCompiledSdkVersion},
    {"SysMemUserForUser", 0xFC114573u, "sceKernelGetCompiledSdkVersion", &GetCompiledSdkVersion},
    {"SysMemUserForUser", 0x13A5ABEFu, "sceKernelPrintf", &Printf},
};

} // namespace

std::span<const HleFunction> sysmem_functions() { return kFunctions; }

} // namespace mhf::psp::hle
