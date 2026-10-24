#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

namespace ghostdrive::core {

// Hybrid logical clock. Timestamps pack 48 bits of milliseconds since the Unix epoch with a
// 16-bit logical counter, so they compare as plain integers and never go backwards locally.
// Used only to tie-break concurrent versions; ordering of history comes from version vectors.
class HybridClock {
public:
    using PhysicalClock = std::function<std::uint64_t()>;  // milliseconds since the Unix epoch

    HybridClock();
    explicit HybridClock(PhysicalClock physical);

    std::uint64_t now();
    // Merges a timestamp received from a peer and returns a local timestamp after both.
    std::uint64_t update(std::uint64_t remote);

    static constexpr std::uint64_t physical_ms(std::uint64_t timestamp) noexcept {
        return timestamp >> 16;
    }
    static constexpr std::uint64_t logical(std::uint64_t timestamp) noexcept {
        return timestamp & 0xFFFF;
    }

private:
    std::uint64_t advance(std::uint64_t floor_ms, std::uint64_t floor_logical);

    PhysicalClock physical_;
    std::mutex mutex_;
    std::uint64_t last_ms_ = 0;
    std::uint64_t last_logical_ = 0;
};

}  // namespace ghostdrive::core
