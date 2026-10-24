#pragma once

#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace ghostdrive::transport {

// Simulated network conditions applied to outgoing datagrams in tests. All randomness comes
// from the seed, so a failing run can be replayed.
struct Impairment {
    std::chrono::microseconds delay{0};
    std::chrono::microseconds jitter{0};  // uniform extra delay in [0, jitter]
    double loss = 0.0;                    // probability a datagram is dropped
    double duplicate = 0.0;               // probability a datagram is sent twice
    double reorder = 0.0;                 // probability a datagram gets reorder_delay extra
    std::chrono::microseconds reorder_delay{std::chrono::milliseconds(10)};
    std::uint64_t seed = 1;

    bool active() const noexcept {
        return delay.count() > 0 || jitter.count() > 0 || loss > 0.0 || duplicate > 0.0 ||
               reorder > 0.0;
    }
};

// Decides, per datagram, how many copies to send and when.
class Impairer {
public:
    explicit Impairer(const Impairment& config) : config_(config), rng_(config.seed) {}

    // Send delays for one datagram: empty when it is dropped, two entries when duplicated.
    std::vector<std::chrono::microseconds> plan();

private:
    std::chrono::microseconds one_delay();

    Impairment config_;
    std::mt19937_64 rng_;
};

}  // namespace ghostdrive::transport
