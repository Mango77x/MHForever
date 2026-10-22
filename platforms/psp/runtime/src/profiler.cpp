// Optional sampling profiler for the host side (MHF_PROFILE=1, Windows only): every millisecond or so it looks at where
// the registered threads are executing and, every 20 seconds, prints which functions they were in. It is meant to find
// where a heavy scene spends its time (the game's own recompiled code, the GE, the interpolation). Function names come
// from the program's PDB, which has to sit next to the executable.

#include "hle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define MHF_PROFILER 1
#include <windows.h>
#include <dbghelp.h>
#endif

namespace mhf::psp::hle {

#if defined(MHF_PROFILER)

namespace {

struct Target {
    std::string name;
    HANDLE handle{};
    std::vector<std::uint64_t> samples;
};

std::mutex g_mutex;
std::vector<Target> g_targets;
bool g_started = false;

std::string symbol_name(HANDLE process, std::uint64_t address, std::unordered_map<std::uint64_t, std::string> &cache) {
    const auto found = cache.find(address);
    if (found != cache.end()) return found->second;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string name;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        name = symbol->Name;
    } else {
        char text[32];
        std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(address));
        name = text;
    }
    cache.emplace(address, name);
    return name;
}

void report(HANDLE process, std::unordered_map<std::uint64_t, std::string> &cache) {
    std::lock_guard lock(g_mutex);
    for (Target &t : g_targets) {
        if (t.samples.empty()) continue;
        // Samples taken while the thread was waiting say nothing about its work: they are counted apart.
        static const char *const waiting[] = {"ZwDelayExecution", "RtlDelayExecution", "ZwWaitForAlertByThreadId",
                                              "NtWaitForSingleObject", "ZwWaitForSingleObject", "SwitchToThread",
                                              "NtWaitForMultipleObjects", "ZwWaitForMultipleObjects"};
        std::unordered_map<std::string, std::size_t> by_function;
        std::size_t busy = 0;
        for (const std::uint64_t address : t.samples) {
            const std::string name = symbol_name(process, address, cache);
            if (std::find_if(std::begin(waiting), std::end(waiting), [&](const char *w) { return name == w; }) ==
                std::end(waiting)) {
                ++busy;
                ++by_function[name];
            }
        }
        std::vector<std::pair<std::string, std::size_t>> sorted(by_function.begin(), by_function.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::fprintf(stderr, "[profile] %s thread: %.0f%% of the time busy (%zu of %zu samples); where it was busy:\n",
                     t.name.c_str(), 100.0 * static_cast<double>(busy) / static_cast<double>(t.samples.size()), busy,
                     t.samples.size());
        for (std::size_t i = 0; i < sorted.size() && i < 16; ++i) {
            std::fprintf(stderr, "[profile]   %5.1f%%  %s\n",
                         100.0 * static_cast<double>(sorted[i].second) / static_cast<double>(std::max<std::size_t>(busy, 1)),
                         sorted[i].first.c_str());
        }
        t.samples.clear();
    }
}

void sampler_main() {
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    std::unordered_map<std::uint64_t, std::string> cache;
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        {
            std::lock_guard lock(g_mutex);
            for (Target &t : g_targets) {
                // Nothing may allocate or lock between Suspend and Resume: the thread could hold the heap lock.
                std::uint64_t address = 0;
                if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) continue;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(t.handle, &context)) address = context.Rip;
                ResumeThread(t.handle);
                if (address != 0) t.samples.push_back(address);
            }
        }
        if (std::chrono::steady_clock::now() >= next_report) {
            report(process, cache);
            next_report = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        }
    }
}

} // namespace

void profiler_register_thread(const char *name) {
    static const bool enabled = std::getenv("MHF_PROFILE") != nullptr;
    if (!enabled) return;
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        return;
    }
    std::lock_guard lock(g_mutex);
    g_targets.push_back(Target{name, handle, {}});
    if (!g_started) {
        g_started = true;
        std::thread(sampler_main).detach();
    }
}

#else

void profiler_register_thread(const char *) {}

#endif

} // namespace mhf::psp::hle
