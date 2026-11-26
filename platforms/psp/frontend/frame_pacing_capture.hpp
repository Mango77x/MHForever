#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace mhf::psp {

// The menu starts a run and collects its result on reopening. Only the drawing thread records swaps.
class FramePacingCapture {
public:
    using Clock = std::chrono::steady_clock;

    struct Result {
        double seconds, fps, p99_ms, worst_ms;
        std::size_t intervals, interpolated;
    };

    void start(Clock::time_point requested) {
        std::lock_guard lock(mutex_);
        requested_ = requested;
        last_.reset();
        intervals_.clear();
        intervals_.reserve(1800); // ten seconds at the highest selectable output rate
        interpolated_ = 0;
        active_.store(true, std::memory_order_relaxed);
    }

    void presented(Clock::time_point now, bool interpolated) {
        if (!active_.load(std::memory_order_relaxed)) return;
        std::lock_guard lock(mutex_);
        if (!active_.load(std::memory_order_relaxed) || now < requested_) return;
        if (!last_) {
            first_ = now;
        } else {
            if (now <= *last_) return;
            intervals_.push_back(now - *last_);
            if (interpolated) ++interpolated_;
            if (now - first_ >= std::chrono::seconds(10)) active_.store(false, std::memory_order_relaxed);
        }
        last_ = now;
    }

    std::optional<Result> finish() {
        std::lock_guard lock(mutex_);
        active_.store(false, std::memory_order_relaxed);
        if (intervals_.empty()) return std::nullopt;
        const double seconds = std::chrono::duration<double>(*last_ - first_).count();
        std::sort(intervals_.begin(), intervals_.end());
        const std::size_t n = intervals_.size();
        using Milliseconds = std::chrono::duration<double, std::milli>;
        // Nearest-rank P99: the smallest interval at or above the 99th percentile.
        return Result{seconds, static_cast<double>(n) / seconds,
                      Milliseconds(intervals_[n - 1 - n / 100]).count(), Milliseconds(intervals_.back()).count(),
                      n, interpolated_};
    }

private:
    std::atomic<bool> active_{false}; // idle swaps need only this check
    std::mutex mutex_;
    Clock::time_point requested_{}, first_{};
    std::optional<Clock::time_point> last_;
    std::vector<Clock::duration> intervals_;
    std::size_t interpolated_{};
};

} // namespace mhf::psp
