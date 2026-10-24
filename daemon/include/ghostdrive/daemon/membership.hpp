#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/daemon/codec.hpp"

namespace ghostdrive::daemon {

struct MembershipView {
    // Members other than this device, with their names.
    std::map<core::PublicKey, std::string> members;
    // Devices taken out of the drive: removed by a member, now or before.
    std::set<core::PublicKey> removed;
    // Of `removed`, those not yet removed for good here (see Membership::countersign).
    std::set<core::PublicKey> removal_pending;
    // Devices in a contest nobody settled (not members). `ghostdrive remove` on one settles it.
    std::set<core::PublicKey> in_doubt;
    // A name for every device known here other than this one.
    std::map<core::PublicKey, std::string> names;
    // For each device, the devices that vouched for it, and those of them that invited it.
    std::map<core::PublicKey, std::set<core::PublicKey>> vouched_by;
    std::map<core::PublicKey, std::set<core::PublicKey>> invited_by;
    // A member that removed this device. This device does not act on it (the others refuse a
    // removed device anyway), but `ghostdrive status` says so.
    std::optional<core::PublicKey> removed_by;
};

// The membership of a drive as one device sees it (ARCHITECTURE.md section 3.6): a set of
// signed entries from which each device computes its own view.
//
// Every known device is a member, out, or in doubt, settled to a fixpoint:
//  - this device is a member;
//  - a device is out if it was ever removed here, if a member removes it, if every device that
//    vouches for it is out, or if no chain of vouches through devices that are not out leads
//    to it from this device;
//  - a device is a member if a member vouches for it and every device that removed it is out;
//  - whatever is still in doubt is not a member.
// So a remove counts only when its author is a member, and a device that a removed device
// vouches for (even one it makes up afterwards) is out. Removes of this device never count
// here, so no member can make it remove itself; its own removes always count, which is how the
// user settles a contest.
//
// Removal is permanent: a device that has stood removed by a member for a while (at once if
// this device removed it) is recorded here for good, and this device signs a remove of its own
// for it, so later changes (such as removing the remover) cannot bring it back, here or on
// devices that hear of both at once.
//
// Pairing makes the first link both ways (the inviter adds the new device; the new device adds
// the inviter, whose key the invite pinned), and each device then vouches for every member it
// accepts, so members stay members when the device that invited them is removed.
//
// Bounded: see kMaxDevices and the limits next to it in codec.hpp. The first entry for a
// statement wins. Not thread-safe.
class Membership {
public:
    Membership(const DriveId& drive, const core::PublicKey& self);

    // The stored form, as written by serialize(); entries are verified again.
    static core::Result<Membership> parse(std::span<const std::byte> data, const DriveId& drive,
                                          const core::PublicKey& self);
    core::Bytes serialize() const;

    // Signs and records an entry by this device.
    core::Status add(const core::DeviceKey& key, const core::PublicKey& subject,
                     std::string name, bool invited);
    core::Status remove(const core::DeviceKey& key, const core::PublicKey& subject);

    // Records entries from another device, in the order it sent them. Entries with a bad
    // signature or form, over a bound, or whose author is out here (or not reachable, also
    // through earlier entries of the same call) are dropped. Returns how many were new.
    std::size_t merge(std::span<const MembershipEntry> entries);

    // Records devices removed by a member as removed for good and signs this device's removes
    // for them, then vouches for every member this device has not vouched for yet. Both wait
    // until `ready(device, removal)` is true (when given), except removals this device made.
    // Returns how many changes it made.
    core::Result<std::size_t> countersign(
        const core::DeviceKey& key,
        const std::function<bool(const core::PublicKey&, bool removal)>& ready = nullptr);

    MembershipView view() const;

    // Entries recorded after sequence `after`, oldest first, at most `limit`; `last` is set to
    // the sequence of the last one returned. Sequences are local and grow on every change.
    std::vector<MembershipEntry> since(std::uint64_t after, std::size_t limit,
                                       std::uint64_t& last) const;
    std::vector<MembershipEntry> entries() const;
    std::uint64_t last_sequence() const noexcept { return sequence_; }

private:
    using Key = std::tuple<MembershipEntry::Kind, core::PublicKey, core::PublicKey>;
    struct Stored {
        MembershipEntry entry;
        std::uint64_t sequence = 0;
    };
    enum class State { open, in, out };
    struct Settled {
        std::map<core::PublicKey, State> state;
        std::set<core::PublicKey> removed_by_member;  // out because a member removed it
        MembershipView view;
    };

    bool well_formed(const MembershipEntry& entry) const;
    // `live` is the number of known devices that are not out.
    bool insert(MembershipEntry entry, const std::map<core::PublicKey, State>* states,
                std::size_t& live);
    core::Status sign_and_insert(const core::DeviceKey& key, MembershipEntry entry,
                                 const Settled& settled, std::size_t& live);
    void compact();
    Settled settle() const;
    std::size_t live_count(const Settled& settled) const;

    DriveId drive_{};
    core::PublicKey self_{};
    std::map<Key, Stored> entries_;
    std::map<core::PublicKey, std::size_t> per_author_;
    std::set<core::PublicKey> known_;
    std::set<core::PublicKey> ever_removed_;
    // Entries verified but not stored (yet), so a resend is not verified again. Bounded.
    std::map<Key, MembershipEntry> verified_;
    std::uint64_t sequence_ = 0;
    std::pair<std::uint64_t, std::size_t> compacted_at_{~std::uint64_t{0}, 0};
};

}  // namespace ghostdrive::daemon
