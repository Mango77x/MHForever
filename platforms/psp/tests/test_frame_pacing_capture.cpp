#include "check.hpp"
#include "frame_pacing_capture.hpp"

#include <chrono>
#include <cmath>

using mhf::psp::FramePacingCapture;
using namespace std::chrono_literals;

int main() {
    FramePacingCapture capture;
    const auto start = FramePacingCapture::Clock::time_point{};
    capture.presented(start, true);
    MHF_CHECK(!capture.finish());

    // A swap already in flight can reach the collector after the next run was requested.
    capture.start(start);
    capture.presented(start - 1ms, false);
    capture.presented(start + 200ms, true); // baseline only
    capture.presented(start + 200ms, false);
    MHF_CHECK(!capture.finish());

    capture.start(start);
    for (int i = 0; i <= 1001; ++i) capture.presented(start + i * 10ms, i % 2 == 1);
    const auto steady = capture.finish();
    MHF_CHECK(steady.has_value());
    if (steady) {
        MHF_CHECK(steady->intervals == 1000 && steady->interpolated == 500);
        MHF_CHECK(steady->seconds == 10.0 && steady->fps == 100.0);
        MHF_CHECK(steady->p99_ms == 10.0 && steady->worst_ms == 10.0);
    }

    // Opening the menu early: keep completed intervals and their actual duration.
    // A single long stall in 100 intervals affects the average and worst, but falls above P99.
    capture.start(start);
    for (int i = 0; i <= 99; ++i) capture.presented(start + i * 10ms, false);
    capture.presented(start + 1990ms, false);
    const auto stutter = capture.finish();
    MHF_CHECK(stutter.has_value());
    if (stutter) {
        MHF_CHECK(stutter->intervals == 100 && stutter->interpolated == 0);
        MHF_CHECK(std::abs(stutter->fps - 100.0 / 1.99) < 0.0001);
        MHF_CHECK(stutter->p99_ms == 10.0 && stutter->worst_ms == 1000.0);
    }

    // A stall crossing the deadline must count in full. Later swaps stay outside the window.
    capture.start(start);
    capture.presented(start + 1s, false);
    capture.presented(start + 13s, false);
    capture.presented(start + 14s, true);
    const auto stall = capture.finish();
    MHF_CHECK(stall.has_value());
    if (stall) {
        MHF_CHECK(stall->intervals == 1 && stall->interpolated == 0);
        MHF_CHECK(stall->seconds == 12.0 && stall->p99_ms == 12000.0 && stall->worst_ms == 12000.0);
    }
    return mhf::test::finish("frame pacing capture");
}
