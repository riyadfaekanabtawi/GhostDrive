#include "ghostdrive/transport/impairment.hpp"

namespace ghostdrive::transport {

std::chrono::microseconds Impairer::one_delay() {
    auto delay = config_.delay;
    if (config_.jitter.count() > 0) {
        std::uniform_int_distribution<std::int64_t> jitter(0, config_.jitter.count());
        delay += std::chrono::microseconds(jitter(rng_));
    }
    if (config_.reorder > 0.0 && std::bernoulli_distribution(config_.reorder)(rng_)) {
        delay += config_.reorder_delay;
    }
    return delay;
}

std::vector<std::chrono::microseconds> Impairer::plan() {
    std::vector<std::chrono::microseconds> sends;
    if (config_.loss > 0.0 && std::bernoulli_distribution(config_.loss)(rng_)) return sends;
    sends.push_back(one_delay());
    if (config_.duplicate > 0.0 && std::bernoulli_distribution(config_.duplicate)(rng_)) {
        sends.push_back(one_delay());
    }
    return sends;
}

}  // namespace ghostdrive::transport
