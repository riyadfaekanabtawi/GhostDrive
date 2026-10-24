#include "ghostdrive/core/hlc.hpp"

#include <algorithm>
#include <chrono>

namespace ghostdrive::core {

namespace {

constexpr std::uint64_t kLogicalMax = 0xFFFF;

std::uint64_t system_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace

HybridClock::HybridClock() : HybridClock(system_ms) {}

HybridClock::HybridClock(PhysicalClock physical) : physical_(std::move(physical)) {}

std::uint64_t HybridClock::advance(std::uint64_t floor_ms, std::uint64_t floor_logical) {
    const std::uint64_t pt = physical_();
    const std::uint64_t ms = std::max({last_ms_, floor_ms, pt});
    std::uint64_t logical = 0;
    if (ms == last_ms_ && ms == floor_ms) {
        logical = std::max(last_logical_, floor_logical) + 1;
    } else if (ms == last_ms_) {
        logical = last_logical_ + 1;
    } else if (ms == floor_ms) {
        logical = floor_logical + 1;
    }
    if (logical > kLogicalMax) {
        // Counter exhausted within one millisecond: borrow the next millisecond.
        last_ms_ = ms + 1;
        last_logical_ = 0;
    } else {
        last_ms_ = ms;
        last_logical_ = logical;
    }
    return (last_ms_ << 16) | last_logical_;
}

std::uint64_t HybridClock::now() {
    std::lock_guard lock(mutex_);
    return advance(0, 0);
}

std::uint64_t HybridClock::update(std::uint64_t remote) {
    std::lock_guard lock(mutex_);
    return advance(physical_ms(remote), logical(remote));
}

}  // namespace ghostdrive::core
