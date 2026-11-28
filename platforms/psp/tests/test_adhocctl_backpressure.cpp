// Exercise the AemuLink worker with a control transport that temporarily cannot write.
#include "adhoc_link_aemu.hpp"
#include "adhocctl.h"
#include "check.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace {

enum class Call { Connect, Scan, Disconnect };
std::mutex call_mutex;
std::condition_variable call_ready;
std::vector<Call> calls;

void record(Call call) {
    calls.push_back(call);
    call_ready.notify_all();
}

} // namespace

void *create_adhocctl_v1_session_v4(const adhocctl_addr_v4 *, const adhocctl_addr_v4 *, const char *, const char *,
                                    const char *, bool, bool (*)(void *), void *) {
    return &calls;
}

void destroy_adhocctl_session(void *) {}

adhocctl_call_status adhocctl_connect(void *, const char *) {
    std::lock_guard lock(call_mutex);
    record(Call::Connect);
    return calls.size() < 3 ? ADHOCCTL_CALL_WOULDBLOCK : ADHOCCTL_CALL_SUCCESS;
}

adhocctl_call_status adhocctl_scan(void *) {
    std::lock_guard lock(call_mutex);
    record(Call::Scan);
    return ADHOCCTL_CALL_SUCCESS;
}

adhocctl_call_status adhocctl_disconnect(void *) {
    std::lock_guard lock(call_mutex);
    record(Call::Disconnect);
    return ADHOCCTL_CALL_SUCCESS;
}

adhocctl_call_status adhocctl_ping(void *) { return ADHOCCTL_CALL_SUCCESS; }

void adhocctl_get_event(void *, adhocctl_event *event) { event->type = ADHOCCTL_CLIENT_EVENT_WOULD_BLOCK; }

int main() {
    mhf::psp::adhoc::AemuLink::Config config;
    config.host = "127.0.0.1";
    mhf::psp::adhoc::AemuLink link(config);
    link.request_connect({});
    link.request_scan();
    link.request_disconnect();

    std::unique_lock lock(call_mutex);
    MHF_CHECK(call_ready.wait_for(lock, std::chrono::seconds(2), [] {
        return !calls.empty() && calls.back() == Call::Disconnect;
    }));
    const std::vector expected{Call::Connect, Call::Connect, Call::Connect, Call::Scan, Call::Disconnect};
    MHF_CHECK(calls == expected);
    return mhf::test::finish("adhocctl_backpressure");
}
