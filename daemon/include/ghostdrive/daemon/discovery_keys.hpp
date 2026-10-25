#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/daemon/codec.hpp"

namespace ghostdrive::daemon {

// The keys that LAN IDs and the rendezvous group are derived from (ARCHITECTURE.md section
// 3.7). A removed device knows the drive secret and every key made before its removal, so each
// device, whenever the set of removed devices it sees changes, makes a new key one generation
// above every key it holds, and members send each other the keys they hold over their
// connections. The current key is the one with the highest (generation, author) among keys
// made by this device or by members; the drive secret is generation 0.
//
// Keys by a device stop counting here as soon as it is not a member, and keys are only taken
// from and sent to members. So a removed device can learn a new key only from a device that
// has not heard of its removal yet, and that device makes a newer one once it does: the last
// keys made are by devices that knew, and never reach it.
//
// A received key may be at most kMaxDiscoveryGenerationStep above the highest held, so a device
// cannot use up the generations before its removal and leave nobody able to make a newer one.
//
// The last few keys that were current here are kept too: a member that was away while the key
// changed still uses one of them, so the daemon looks for it in their rendezvous groups.
//
// Bounded by kMaxDiscoveryKeys (one key per author) plus kMaxEarlierKeys. Not thread-safe.
class DiscoveryKeys {
public:
    DiscoveryKeys(const DriveId& drive, const core::PublicKey& self, const core::DriveSecret& base);
    ~DiscoveryKeys();
    DiscoveryKeys(DiscoveryKeys&& other) noexcept;
    DiscoveryKeys& operator=(DiscoveryKeys&& other) noexcept;
    DiscoveryKeys(const DiscoveryKeys&) = delete;
    DiscoveryKeys& operator=(const DiscoveryKeys&) = delete;

    // The stored form, as written by serialize(); signatures are verified again.
    static core::Result<DiscoveryKeys> parse(std::span<const std::byte> data, const DriveId& drive,
                                             const core::PublicKey& self,
                                             const core::DriveSecret& base);
    // Holds secrets: the caller zeroes it.
    core::Bytes serialize() const;

    struct Held {
        std::uint64_t generation = 0;
        const core::DriveSecret* key = nullptr;
    };
    static constexpr std::size_t kMaxEarlierKeys = 3;

    const core::DriveSecret& current() const noexcept;
    // Which key is current: its generation and author (all zero for the drive secret).
    std::pair<std::uint64_t, core::PublicKey> current_id() const noexcept;
    // Every key held, the current one first and the drive secret last: a device that has not
    // caught up yet still advertises an older one.
    std::vector<Held> all() const;
    // The keys that were current here before, newest first, then the drive secret; never the
    // current key.
    std::vector<const core::DriveSecret*> earlier() const;
    // The keys to send to a member (not the drive secret). The caller zeroes them.
    std::vector<DiscoveryKey> keys() const;
    // Grows on every change.
    std::uint64_t version() const noexcept { return version_; }

    // Drops keys by devices that are not members. Then, if `removed` is not the set of removed
    // devices this device last made a key for, makes a new one. Returns whether anything changed.
    core::Result<bool> update(const core::DeviceKey& key, const std::set<core::PublicKey>& members,
                              const std::set<core::PublicKey>& removed);
    // Records keys sent by a member. Keys by devices that are not members, older than the one
    // held from the same author, or with a bad signature are dropped. Returns whether any were new.
    bool merge(std::span<const DiscoveryKey> keys, const std::set<core::PublicKey>& members);
    // Records that the keys held already account for `removed` (when joining a drive).
    void set_removed(const std::set<core::PublicKey>& removed);

private:
    using Digest = std::array<std::byte, 32>;
    static core::Result<Digest> digest(const std::set<core::PublicKey>& removed);
    bool prune(const std::set<core::PublicKey>& members);
    const DiscoveryKey* best() const;
    // Keeps `before` among the earlier keys if it is no longer current.
    void remember(const std::optional<DiscoveryKey>& before);
    std::optional<DiscoveryKey> best_copy() const;
    void wipe() noexcept;

    DriveId drive_{};
    core::PublicKey self_{};
    core::DriveSecret base_{};
    std::map<core::PublicKey, DiscoveryKey> by_author_;
    std::deque<DiscoveryKey> earlier_;
    Digest removed_digest_{};
    std::uint64_t version_ = 0;
};

}  // namespace ghostdrive::daemon
