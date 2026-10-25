#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <string>

namespace ghostdrive::discovery {

// Advertisements resolved recently, so that a browse which reports the same names again and
// again (Windows DNS-SD does, once per response) resolves each only once in a while. Entries
// expire, so a peer's rotated ID or new address is resolved again, and a full table drops its
// oldest entry, so a host flooding the LAN with names cannot shut new ones out.
class SeenNames {
public:
    using Clock = std::chrono::steady_clock;

    SeenNames(std::size_t capacity, Clock::duration lifetime)
        : capacity_(capacity), lifetime_(lifetime) {}

    // True if `name` is due for a resolve: never seen, or seen longer than `lifetime` ago.
    bool due(const std::string& name, Clock::time_point now) const {
        auto it = seen_.find(name);
        return it == seen_.end() || now - it->second >= lifetime_;
    }

    // Records that `name` is being resolved at `now`, dropping the oldest entry if full.
    void remember(const std::string& name, Clock::time_point now) {
        if (auto it = seen_.find(name); it != seen_.end()) {
            it->second = now;
            return;
        }
        if (capacity_ == 0) return;
        if (seen_.size() >= capacity_) {
            auto oldest = seen_.begin();
            for (auto it = seen_.begin(); it != seen_.end(); ++it) {
                if (it->second < oldest->second) oldest = it;
            }
            seen_.erase(oldest);
        }
        seen_.emplace(name, now);
    }

    // After a goodbye or a failed resolve, so the next report of the name resolves it again.
    void forget(const std::string& name) { seen_.erase(name); }

    std::size_t size() const { return seen_.size(); }

private:
    std::size_t capacity_;
    Clock::duration lifetime_;
    std::map<std::string, Clock::time_point> seen_;
};

}  // namespace ghostdrive::discovery
