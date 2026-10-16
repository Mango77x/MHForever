#include "mhf/psp/runtime.hpp"

#include "hle.hpp"
#include "kernel.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mhf::psp {

namespace {
Runtime *g_active = nullptr;

std::uint64_t hle_key(std::string_view library, std::uint32_t nid) {
    return (static_cast<std::uint64_t>(std::hash<std::string_view>{}(library)) << 32u) ^ nid;
}

constexpr int kBootThreadPriority = 0x20;
constexpr std::uint32_t kBootThreadStack = 0x40000u;
} // namespace

Runtime *Runtime::active() noexcept { return g_active; }

Runtime::Runtime(RuntimeOptions options)
    : options_(std::move(options)), memory_(options_.ram_size), kernel_(std::make_unique<Kernel>(*this)) {
    g_active = this;
    add_hle(hle::thread_functions());
    add_hle(hle::sysmem_functions());
    add_hle(hle::io_functions());
    add_hle(hle::system_functions());
    add_hle(hle::display_functions());
    add_hle(hle::misc_functions());
    add_hle(hle::ge_functions());
    add_hle(hle::media_functions());
}

Runtime::~Runtime() {
    if (g_active == this) {
        g_active = nullptr;
    }
}

void Runtime::add_program(const Program &program) {
    functions_.insert(functions_.end(), program.functions.begin(), program.functions.end());
    std::sort(functions_.begin(), functions_.end(),
              [](const FunctionEntry &a, const FunctionEntry &b) { return a.address < b.address; });
}

void Runtime::add_hle(std::span<const HleFunction> functions) {
    for (const auto &f : functions) {
        hle_[hle_key(f.library, f.nid)] = &f;
    }
}

void Runtime::add_overlay(const Program &overlay) {
    overlays_[overlay.overlay_id] = &overlay;
    for (auto &region : overlay_regions_) {
        if (region.start == overlay.code_start || (overlay.code_start < region.end && overlay.code_end > region.start)) {
            region.start = std::min(region.start, overlay.code_start);
            region.end = std::max(region.end, overlay.code_end);
            return;
        }
    }
    overlay_regions_.push_back({overlay.code_start, overlay.code_end, nullptr});
}

bool Runtime::activate_overlay(std::int32_t overlay_id) {
    const auto it = overlays_.find(overlay_id);
    if (it == overlays_.end()) {
        return false;
    }
    for (auto &region : overlay_regions_) {
        if (it->second->code_start >= region.start && it->second->code_start < region.end) {
            region.active = it->second;
            ++overlay_activations_;
            return true;
        }
    }
    return false;
}

const Program *Runtime::active_overlay_at(std::uint32_t address) const noexcept {
    for (const auto &region : overlay_regions_) {
        if (address >= region.start && address < region.end) {
            return region.active;
        }
    }
    return nullptr;
}

void Runtime::register_hook(std::string name, HookFunction function) { hooks_[std::move(name)] = function; }

HookFunction Runtime::find_hook(std::string_view name) const {
    const auto it = hooks_.find(std::string(name));
    return it != hooks_.end() ? it->second : nullptr;
}

const HleFunction *Runtime::find_hle(std::string_view library, std::uint32_t nid) const {
    const auto it = hle_.find(hle_key(library, nid));
    return it != hle_.end() ? it->second : nullptr;
}

void Runtime::load_elf(const std::filesystem::path &path) {
    const auto elf = psprecomp::Elf32Image::from_file(path);
    if (elf.is_psp_prx()) {
        throw std::runtime_error("relocatable modules are not supported yet: " + path.string());
    }
    elf.load_into(memory_, 0u);
    for (const auto &segment : elf.segments()) {
        if (segment.type == 1u && segment.memory_size != 0) {
            const int partition = segment.vaddr >= SysMem::kUserPartitionStart ? 2 : 1;
            if (!kernel_->sysmem().reserve(partition, "module", segment.vaddr, segment.memory_size)) {
                throw std::runtime_error("module segment overlaps reserved memory");
            }
        }
    }
}

int Runtime::run(const Program &program, std::string_view boot_path) {
    add_program(program);
    kernel_->set_module_gp(program.gp);
    if (const auto slash = boot_path.rfind('/'); slash != std::string_view::npos) {
        hle::set_working_directory(*this, std::string(boot_path.substr(0, slash)));
    }
    // Boot arguments: the path of the executable, as the PSP loader passes it.
    const std::string path{boot_path};
    const auto args = kernel_->sysmem().allocate(1, "bootargs", SysMem::Placement::Low,
                                                 static_cast<std::uint32_t>(path.size() + 1));
    if (!args) {
        throw std::runtime_error("cannot allocate boot arguments");
    }
    memory_.zero(args->address, args->size);
    memory_.copy_in(args->address, std::span(reinterpret_cast<const std::uint8_t *>(path.data()), path.size()));
    const int uid = kernel_->create_thread("root", program.entry, kBootThreadPriority, kBootThreadStack, 0);
    if (uid < 0) {
        throw std::runtime_error("cannot create the boot thread");
    }
    kernel_->start_thread(uid, static_cast<std::uint32_t>(path.size() + 1), args->address);
    return kernel_->run_scheduler();
}

namespace {
GuestFunction find_in(std::span<const FunctionEntry> table, std::uint32_t address) {
    const auto it = std::lower_bound(table.begin(), table.end(), address,
                                     [](const FunctionEntry &e, std::uint32_t a) { return e.address < a; });
    return it != table.end() && it->address == address ? it->function : nullptr;
}
} // namespace

GuestFunction Runtime::lookup(std::uint32_t address) const noexcept {
    if (!overlay_regions_.empty()) {
        for (const auto &region : overlay_regions_) {
            if (address >= region.start && address < region.end) {
                return region.active != nullptr ? find_in(region.active->functions, address) : nullptr;
            }
        }
    }
    return find_in(functions_, address);
}

void Runtime::call(Context &ctx, std::uint32_t address) {
    const GuestFunction f = lookup(address & 0x3FFFFFFFu);
    if (f == nullptr) {
        const Program *overlay = active_overlay_at(address & 0x3FFFFFFFu);
        fatal("no recompiled function at " + psprecomp::hex32(address) + " (called from $ra=" +
              psprecomp::hex32(ctx.gpr[31]) + (overlay != nullptr ? std::string(", active overlay ") + overlay->name
                                                                    : std::string()) + ")");
    }
    f(*this, ctx);
}

void Runtime::unsupported(std::uint32_t pc, std::uint32_t word, const char *reason) {
    fatal("unsupported instruction " + psprecomp::hex32(word) + " at " + psprecomp::hex32(pc) + ": " + reason);
}

void Runtime::arithmetic_overflow(std::uint32_t pc, std::uint32_t word) {
    fatal("integer overflow exception at " + psprecomp::hex32(pc) + " (" + psprecomp::hex32(word) + ")");
}

void Runtime::fatal(const std::string &message) {
    std::fprintf(stderr, "[fatal] %s\n", message.c_str());
    if (auto *t = kernel_->current()) {
        std::fprintf(stderr, "[fatal] in thread %d '%s'\n", t->uid, t->name.c_str());
    }
    std::fflush(stderr);
    exit_program(70);
}

void Runtime::exit_program(int code) {
    std::fflush(stdout);
    kernel_->exit_program(code);
}

void Runtime::kernel_host_poll(std::function<void()> poll) { kernel_->host_poll = std::move(poll); }
std::uint64_t Runtime::now_us() const { return kernel_->now_us(); }
void Runtime::exit_program_from_host(int code) { kernel_->request_exit(code); }

void Runtime::log_unimplemented(const ImportEntry &import) {
    const auto key = hle_key(import.library, import.nid);
    if (!reported_unimplemented_[key]) {
        reported_unimplemented_[key] = true;
        std::fprintf(stderr, "[hle] unimplemented %s %s (returns 0)\n", import.library,
                     psprecomp::hex32(import.nid).c_str());
    }
}

void call_import(Runtime &rt, Context &ctx, ImportEntry &import) {
    if (import.hle == nullptr) {
        import.hle = rt.find_hle(import.library, import.nid);
    }
    if (import.hle == nullptr) {
        rt.log_unimplemented(import);
        ctx.gpr[2] = 0;
        return;
    }
    if (rt.options().trace_imports) {
        std::fprintf(stderr, "[hle] %s(%08X, %08X, %08X, %08X)\n", import.hle->name, ctx.gpr[4], ctx.gpr[5],
                     ctx.gpr[6], ctx.gpr[7]);
    }
    import.hle->handler(rt, ctx);
}

namespace hle {
std::string read_string(Runtime &rt, std::uint32_t address, std::size_t max_length) {
    if (address == 0) {
        return {};
    }
    return rt.memory().read_c_string(address, max_length);
}
} // namespace hle

} // namespace mhf::psp
