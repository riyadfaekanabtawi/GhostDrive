#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/result.hpp"

namespace ghostdrive::core {

struct Counter {
    std::uint64_t device;
    std::uint64_t value;

    friend bool operator==(const Counter&, const Counter&) = default;
};

// Per-file version vector keyed by device short ID. Kept sorted by device with no zero entries,
// so equal histories always have equal representations.
class VersionVector {
public:
    enum class Order { equal, before, after, concurrent };

    void bump(std::uint64_t device);
    std::uint64_t get(std::uint64_t device) const noexcept;

    // Relation of *this to other: `before` means other has seen everything *this has.
    Order compare(const VersionVector& other) const noexcept;
    VersionVector merged(const VersionVector& other) const;

    const std::vector<Counter>& counters() const noexcept { return counters_; }

    // Little-endian: u32 count, then count pairs of (u64 device, u64 value).
    Bytes serialize() const;
    static Result<VersionVector> parse(std::span<const std::byte> data);

    friend bool operator==(const VersionVector&, const VersionVector&) = default;

private:
    std::vector<Counter> counters_;
};

}  // namespace ghostdrive::core
