#include "ghostdrive/core/version_vector.hpp"

#include <algorithm>
#include <limits>

namespace ghostdrive::core {

namespace {

void put_u64(Bytes& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>(value >> (8 * i)));
}

std::uint64_t get_u64(std::span<const std::byte> in) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | std::to_integer<std::uint64_t>(in[static_cast<std::size_t>(i)]);
    }
    return value;
}

}  // namespace

void VersionVector::bump(std::uint64_t device) {
    auto it = std::lower_bound(counters_.begin(), counters_.end(), device,
                               [](const Counter& c, std::uint64_t d) { return c.device < d; });
    if (it != counters_.end() && it->device == device) {
        ++it->value;
    } else {
        counters_.insert(it, Counter{device, 1});
    }
}

std::uint64_t VersionVector::get(std::uint64_t device) const noexcept {
    auto it = std::lower_bound(counters_.begin(), counters_.end(), device,
                               [](const Counter& c, std::uint64_t d) { return c.device < d; });
    return it != counters_.end() && it->device == device ? it->value : 0;
}

VersionVector::Order VersionVector::compare(const VersionVector& other) const noexcept {
    bool this_ahead = false;
    bool other_ahead = false;
    auto a = counters_.begin();
    auto b = other.counters_.begin();
    while (a != counters_.end() || b != other.counters_.end()) {
        if (b == other.counters_.end() || (a != counters_.end() && a->device < b->device)) {
            this_ahead = true;
            ++a;
        } else if (a == counters_.end() || b->device < a->device) {
            other_ahead = true;
            ++b;
        } else {
            if (a->value > b->value) this_ahead = true;
            if (b->value > a->value) other_ahead = true;
            ++a;
            ++b;
        }
    }
    if (this_ahead && other_ahead) return Order::concurrent;
    if (this_ahead) return Order::after;
    if (other_ahead) return Order::before;
    return Order::equal;
}

VersionVector VersionVector::merged(const VersionVector& other) const {
    VersionVector out;
    out.counters_.reserve(counters_.size() + other.counters_.size());
    auto a = counters_.begin();
    auto b = other.counters_.begin();
    while (a != counters_.end() || b != other.counters_.end()) {
        if (b == other.counters_.end() || (a != counters_.end() && a->device < b->device)) {
            out.counters_.push_back(*a++);
        } else if (a == counters_.end() || b->device < a->device) {
            out.counters_.push_back(*b++);
        } else {
            out.counters_.push_back(Counter{a->device, std::max(a->value, b->value)});
            ++a;
            ++b;
        }
    }
    return out;
}

Bytes VersionVector::serialize() const {
    Bytes out;
    out.reserve(4 + counters_.size() * 16);
    const auto count = static_cast<std::uint32_t>(counters_.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(count >> (8 * i)));
    for (const Counter& c : counters_) {
        put_u64(out, c.device);
        put_u64(out, c.value);
    }
    return out;
}

Result<VersionVector> VersionVector::parse(std::span<const std::byte> data) {
    if (data.size() < 4) return Error{Errc::corrupt, "version vector: truncated header"};
    std::uint32_t count = 0;
    for (int i = 3; i >= 0; --i) {
        count = (count << 8) | std::to_integer<std::uint32_t>(data[static_cast<std::size_t>(i)]);
    }
    if (data.size() != 4 + static_cast<std::size_t>(count) * 16) {
        return Error{Errc::corrupt, "version vector: length does not match count"};
    }
    VersionVector out;
    out.counters_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto entry = data.subspan(4 + i * 16, 16);
        const Counter c{get_u64(entry.first(8)), get_u64(entry.subspan(8))};
        if (c.value == 0) return Error{Errc::corrupt, "version vector: zero counter"};
        if (!out.counters_.empty() && out.counters_.back().device >= c.device) {
            return Error{Errc::corrupt, "version vector: devices not strictly increasing"};
        }
        out.counters_.push_back(c);
    }
    return out;
}

}  // namespace ghostdrive::core
