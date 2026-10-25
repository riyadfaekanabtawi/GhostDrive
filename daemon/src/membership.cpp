#include "ghostdrive/daemon/membership.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>

namespace ghostdrive::daemon {

using core::Errc;
using core::Error;
using Kind = MembershipEntry::Kind;

namespace {

constexpr std::array<char, 4> kMagic{'G', 'D', 'M', '1'};
// Merge passes per call: a sender's entries come in the order it recorded them, so one or two
// passes place everything; more only serve a sender ordering them badly on purpose.
constexpr int kMaxMergePasses = 8;
constexpr std::size_t kMaxVerified = 4096;

}  // namespace

Membership::Membership(const DriveId& drive, const core::PublicKey& self)
    : drive_(drive), self_(self), known_{self} {}

core::Result<Membership> Membership::parse(std::span<const std::byte> data, const DriveId& drive,
                                           const core::PublicKey& self) {
    auto corrupt = [] { return Error{Errc::corrupt, "the membership file is damaged"}; };
    if (data.size() < kMagic.size() + 4 ||
        std::memcmp(data.data(), kMagic.data(), kMagic.size()) != 0) {
        return corrupt();
    }
    // Layout: magic | u32 removed count | removed keys | membership payload.
    std::size_t pos = kMagic.size();
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < 4; ++i) count |= std::to_integer<std::uint32_t>(data[pos + i]) << (8 * i);
    pos += 4;
    if (count > kMaxEverRemoved || data.size() < pos + count * core::kPublicKeySize) return corrupt();
    Membership out(drive, self);
    for (std::uint32_t i = 0; i < count; ++i) {
        core::PublicKey key{};
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(pos), key.size(), key.begin());
        pos += key.size();
        out.ever_removed_.insert(key);
    }
    auto entries = decode_membership(data.subspan(pos));
    if (!entries) return corrupt();
    // A compacted log keeps entries about keys whose introducing add it dropped (such as this
    // device's removes of devices removed for good), so every key mentioned is known, as
    // compact() leaves it.
    for (const auto& e : entries.value()) {
        if (!out.well_formed(e)) return corrupt();
        out.known_.insert(e.author);
        out.known_.insert(e.subject);
    }
    for (auto& e : entries.value()) {
        std::size_t live = 0;
        out.insert(std::move(e), nullptr, live);
    }
    return out;
}

core::Bytes Membership::serialize() const {
    core::Bytes out(kMagic.size());
    std::memcpy(out.data(), kMagic.data(), kMagic.size());
    auto count = static_cast<std::uint32_t>(ever_removed_.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(count >> (8 * i)));
    for (const auto& key : ever_removed_) out.insert(out.end(), key.begin(), key.end());
    auto all = entries();
    auto payload = encode_membership_payload(all);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool Membership::well_formed(const MembershipEntry& e) const {
    if (e.author == e.subject || e.name.size() > kMaxMemberNameBytes) return false;
    if (e.kind == Kind::remove && (e.invited || !e.name.empty())) return false;
    if (e.kind != Kind::add && e.kind != Kind::remove) return false;
    return core::verify_signature(e.author, membership_signed_bytes(drive_, e), e.signature);
}

bool Membership::insert(MembershipEntry entry, const std::map<core::PublicKey, State>* states,
                        std::size_t& live) {
    // The first entry for a statement wins: an author re-signing its own entries cannot make
    // every device store, forward and act on them again.
    Key key{entry.kind, entry.author, entry.subject};
    if (entries_.contains(key)) return false;
    bool own = entry.author == self_;
    if (!own) {
        if (per_author_[entry.author] >= kMaxEntriesPerAuthor) return false;
        // About a device removed for good, another device's entry changes nothing here, and
        // this device's own remove already tells the others.
        if (ever_removed_.contains(entry.subject)) return false;
    }
    // Room is kept for this device's own entries. A full log first drops what no longer counts.
    auto limit = own ? kMaxMembershipEntries : kMaxMembershipEntries - kMaxOwnEntries;
    if (entries_.size() >= limit || (own && per_author_[self_] >= kMaxOwnEntries)) {
        compact();
        if (entries_.size() >= limit || (own && per_author_[self_] >= kMaxOwnEntries)) {
            return false;
        }
    }
    if (!known_.contains(entry.author)) return false;
    bool introduces = !known_.contains(entry.subject);
    // Only an add introduces a device.
    if (introduces && entry.kind != Kind::add) return false;
    if (entry.kind == Kind::add && !ever_removed_.contains(entry.subject)) {
        if (introduces) {
            auto standing = static_cast<std::size_t>(std::count_if(
                known_.begin(), known_.end(),
                [&](const core::PublicKey& k) { return !ever_removed_.contains(k); }));
            if (standing >= kMaxKnownKeys) return false;
        }
        // Any add that could make an out device live again counts against the live limit,
        // not only one that introduces a key. (`states` is absent only when loading.)
        if (states) {
            auto it = states->find(entry.subject);
            bool live_now = !introduces && it != states->end() && it->second != State::out;
            if (!live_now) {
                if (live >= kMaxDevices) return false;
                ++live;
            }
        }
    }
    if (introduces) known_.insert(entry.subject);
    ++per_author_[entry.author];
    verified_.erase(key);
    entries_.emplace(key, Stored{std::move(entry), ++sequence_});
    return true;
}

void Membership::compact() {
    // Once per change: a full log that frees nothing must not be settled again on every try.
    if (compacted_at_ == std::pair{sequence_, ever_removed_.size()}) return;
    // Entries by devices that are out here affect no view: their vouches and removes do not
    // count. Nor do entries about devices removed for good, except this device's own removes,
    // which tell the others (the newest kMaxKnownKeys of them are kept). Drop the rest and
    // forget keys nothing mentions any more.
    auto settled = settle();
    std::vector<std::pair<std::uint64_t, Key>> own_removes;
    for (const auto& [key, stored] : entries_) {
        const auto& e = stored.entry;
        if (e.author == self_ && e.kind == Kind::remove && ever_removed_.contains(e.subject)) {
            own_removes.emplace_back(stored.sequence, key);
        }
    }
    std::sort(own_removes.begin(), own_removes.end());
    std::set<Key> old_own_removes;
    if (own_removes.size() > kMaxKnownKeys) {
        for (std::size_t i = 0; i < own_removes.size() - kMaxKnownKeys; ++i) {
            old_own_removes.insert(own_removes[i].second);
        }
    }
    std::erase_if(entries_, [&](const auto& item) {
        const auto& e = item.second.entry;
        if (e.author == self_) {
            return (e.kind == Kind::add && ever_removed_.contains(e.subject)) ||
                   old_own_removes.contains(item.first);
        }
        return settled.state[e.author] == State::out || ever_removed_.contains(e.subject);
    });
    per_author_.clear();
    known_ = {self_};
    for (const auto& [key, stored] : entries_) {
        ++per_author_[stored.entry.author];
        known_.insert(stored.entry.author);
        known_.insert(stored.entry.subject);
    }
    compacted_at_ = {sequence_, ever_removed_.size()};
}

core::Status Membership::sign_and_insert(const core::DeviceKey& key, MembershipEntry entry,
                                         const Settled& settled, std::size_t& live) {
    entry.author = key.public_key();
    entry.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    auto signature = key.sign(membership_signed_bytes(drive_, entry));
    if (!signature) return signature.error();
    entry.signature = signature.value();
    if (!insert(std::move(entry), &settled.state, live)) {
        return Error{Errc::invalid_argument, "the drive has no room for another device (at most " +
                                                 std::to_string(kMaxDevices) + " at once, " +
                                                 std::to_string(kMaxKnownKeys) + " ever)"};
    }
    return {};
}

core::Status Membership::add(const core::DeviceKey& key, const core::PublicKey& subject,
                             std::string name, bool invited) {
    if (subject == self_) return Error{Errc::invalid_argument, "a device cannot add itself"};
    if (entries_.contains(Key{Kind::add, self_, subject})) return {};
    MembershipEntry e;
    e.kind = Kind::add;
    e.invited = invited;
    e.subject = subject;
    e.name = std::move(name);
    if (e.name.size() > kMaxMemberNameBytes) e.name.resize(kMaxMemberNameBytes);
    auto settled = settle();
    auto live = live_count(settled);
    return sign_and_insert(key, std::move(e), settled, live);
}

core::Status Membership::remove(const core::DeviceKey& key, const core::PublicKey& subject) {
    if (subject == self_) return Error{Errc::invalid_argument, "a device cannot remove itself"};
    if (!known_.contains(subject)) return Error{Errc::not_found, "no such device in this drive"};
    if (entries_.contains(Key{Kind::remove, self_, subject})) return {};
    MembershipEntry e;
    e.kind = Kind::remove;
    e.subject = subject;
    auto settled = settle();
    std::size_t live = 0;  // a remove never makes a device live
    return sign_and_insert(key, std::move(e), settled, live);
}

std::size_t Membership::merge(std::span<const MembershipEntry> entries) {
    std::vector<const MembershipEntry*> pending;
    for (const auto& e : entries) {
        // Stored entries are skipped, and entries verified before are not verified again.
        Key key{e.kind, e.author, e.subject};
        if (entries_.contains(key)) continue;
        if (auto it = verified_.find(key); it != verified_.end() && it->second == e) {
            pending.push_back(&e);
        } else if (well_formed(e)) {
            pending.push_back(&e);
        }
    }
    std::size_t added = 0;
    for (int pass = 0; pass < kMaxMergePasses && !pending.empty(); ++pass) {
        auto settled = settle();
        auto live = live_count(settled);
        bool progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            auto state = settled.state.find((*it)->author);
            if (state == settled.state.end() || state->second == State::out) {
                ++it;
                continue;
            }
            if (insert(**it, &settled.state, live)) {
                ++added;
                progress = true;
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
        if (!progress) break;
    }
    if (verified_.size() + pending.size() > kMaxVerified) verified_.clear();
    for (const auto* e : pending) verified_.emplace(Key{e->kind, e->author, e->subject}, *e);
    return added;
}

Membership::Settled Membership::settle() const {
    // Every known device starts open and is settled along a worklist with counters, so each
    // round is linear in the number of entries.
    struct Node {
        std::size_t supporters_open = 0;  // vouchers not (yet) out
        std::size_t supporters_in = 0;
        std::size_t removers_open = 0;  // removers not (yet) out
        std::size_t removers_in = 0;
        std::vector<core::PublicKey> supports;
        std::vector<core::PublicKey> removes;
    };
    Settled out;
    std::map<core::PublicKey, Node> nodes;
    for (const auto& key : known_) {
        nodes[key];
        out.state[key] = State::open;
    }
    std::set<core::PublicKey> self_removed_by;
    for (const auto& [key, stored] : entries_) {
        const auto& e = stored.entry;
        if (e.kind == Kind::add) {
            out.view.vouched_by[e.subject].insert(e.author);
            if (e.invited) out.view.invited_by[e.subject].insert(e.author);
            if (e.subject == self_) continue;
            ++nodes[e.subject].supporters_open;
            nodes[e.author].supports.push_back(e.subject);
        } else {
            if (e.subject == self_) {
                self_removed_by.insert(e.author);
                continue;
            }
            ++nodes[e.subject].removers_open;
            nodes[e.author].removes.push_back(e.subject);
        }
    }

    std::deque<core::PublicKey> decided;
    auto decide = [&](const core::PublicKey& key, State state) {
        out.state[key] = state;
        decided.push_back(key);
    };
    auto evaluate = [&](const core::PublicKey& key) {
        if (key == self_ || out.state[key] != State::open) return;
        const auto& n = nodes[key];
        if (n.removers_in > 0) {
            out.removed_by_member.insert(key);
            decide(key, State::out);
        } else if (n.supporters_open == 0) {
            decide(key, State::out);
        } else if (n.removers_open == 0 && n.supporters_in > 0) {
            decide(key, State::in);
        }
    };
    auto drain = [&] {
        while (!decided.empty()) {
            auto key = decided.front();
            decided.pop_front();
            bool in = out.state[key] == State::in;
            const auto& n = nodes[key];
            for (const auto& s : n.supports) {
                if (in)
                    ++nodes[s].supporters_in;
                else
                    --nodes[s].supporters_open;
                evaluate(s);
            }
            for (const auto& s : n.removes) {
                if (in)
                    ++nodes[s].removers_in;
                else
                    --nodes[s].removers_open;
                evaluate(s);
            }
        }
    };
    decide(self_, State::in);
    for (const auto& key : ever_removed_) {
        if (key != self_ && nodes.contains(key)) decide(key, State::out);
    }
    for (const auto& [key, n] : nodes) evaluate(key);
    drain();

    // Devices no chain of vouches through devices that are not out reaches from here are out:
    // a loop of keys vouching for each other is not anchored to anything this device trusts.
    for (;;) {
        std::set<core::PublicKey> reached{self_};
        std::deque<core::PublicKey> queue{self_};
        while (!queue.empty()) {
            auto key = queue.front();
            queue.pop_front();
            for (const auto& s : nodes[key].supports) {
                if (out.state[s] != State::out && reached.insert(s).second) queue.push_back(s);
            }
        }
        bool any = false;
        for (const auto& [key, state] : out.state) {
            if (state == State::open && !reached.contains(key)) {
                decide(key, State::out);
                any = true;
            }
        }
        if (!any) break;
        drain();
    }

    // Names: this device's own vouch (the name it accepted the device under), else an inviting
    // vouch by a member, else any vouch by a member, else any; the smallest author on ties.
    std::map<core::PublicKey, int> name_rank;
    for (const auto& [key, stored] : entries_) {
        const auto& e = stored.entry;
        if (e.kind != Kind::add || e.subject == self_) continue;
        bool member = out.state[e.author] == State::in;
        int rank = e.author == self_ ? 3 : (member && e.invited) ? 2 : member ? 1 : 0;
        auto it = name_rank.find(e.subject);
        if (it != name_rank.end() && it->second >= rank) continue;
        name_rank[e.subject] = rank;
        out.view.names[e.subject] = printable_name(e.name);
    }
    for (const auto& [key, state] : out.state) {
        if (key == self_) continue;
        if (state == State::in) out.view.members[key] = out.view.names[key];
        if (state == State::open) out.view.in_doubt.insert(key);
    }
    out.view.removed = out.removed_by_member;
    out.view.removed.insert(ever_removed_.begin(), ever_removed_.end());
    for (const auto& key : out.removed_by_member) {
        if (!ever_removed_.contains(key)) out.view.removal_pending.insert(key);
    }
    for (const auto& author : self_removed_by) {
        if (out.state[author] == State::in) {
            out.view.removed_by = author;
            break;
        }
    }
    return out;
}

std::size_t Membership::live_count(const Settled& settled) const {
    return static_cast<std::size_t>(
        std::count_if(settled.state.begin(), settled.state.end(),
                      [](const auto& entry) { return entry.second != State::out; }));
}

MembershipView Membership::view() const {
    return settle().view;
}

core::Result<std::size_t> Membership::countersign(
    const core::DeviceKey& key,
    const std::function<bool(const core::PublicKey&, bool removal)>& ready) {
    auto settled = settle();
    auto live = live_count(settled);
    std::size_t changes = 0;
    // A removal becomes permanent here only once it has stood for a while (or this device made
    // it): a remove that reached this device before the remover's own removal did must not
    // turn into a permanent one on its own.
    for (const auto& removed : settled.removed_by_member) {
        if (ever_removed_.contains(removed)) continue;
        bool own = entries_.contains(Key{Kind::remove, self_, removed});
        if (!own && ready && !ready(removed, true)) continue;
        if (ever_removed_.size() >= kMaxEverRemoved) {
            // Sized above every key the log can mention: what can go is no longer mentioned,
            // so nothing vouches for it and only a new add could bring it back.
            std::erase_if(ever_removed_, [&](const core::PublicKey& k) { return !known_.contains(k); });
            if (ever_removed_.size() >= kMaxEverRemoved) continue;
        }
        ever_removed_.insert(removed);
        ++changes;
    }
    for (const auto& removed : ever_removed_) {
        if (removed == self_ || !known_.contains(removed) ||
            entries_.contains(Key{Kind::remove, self_, removed})) {
            continue;
        }
        MembershipEntry e;
        e.kind = Kind::remove;
        e.subject = removed;
        if (auto st = sign_and_insert(key, std::move(e), settled, live); !st) return st.error();
        ++changes;
    }
    for (const auto& [subject, name] : settled.view.members) {
        if (entries_.contains(Key{Kind::add, self_, subject})) continue;
        if (ready && !ready(subject, false)) continue;
        MembershipEntry e;
        e.kind = Kind::add;
        e.subject = subject;
        e.name = name.substr(0, kMaxMemberNameBytes);
        if (auto st = sign_and_insert(key, std::move(e), settled, live); !st) return st.error();
        ++changes;
    }
    return changes;
}

std::vector<MembershipEntry> Membership::since(std::uint64_t after, std::size_t limit,
                                               std::uint64_t& last) const {
    std::vector<const Stored*> newer;
    for (const auto& [key, stored] : entries_) {
        if (stored.sequence > after) newer.push_back(&stored);
    }
    std::sort(newer.begin(), newer.end(),
              [](const Stored* a, const Stored* b) { return a->sequence < b->sequence; });
    if (newer.size() > limit) newer.resize(limit);
    std::vector<MembershipEntry> out;
    out.reserve(newer.size());
    last = after;
    for (const auto* s : newer) {
        out.push_back(s->entry);
        last = s->sequence;
    }
    return out;
}

std::vector<MembershipEntry> Membership::entries() const {
    std::uint64_t last = 0;
    return since(0, entries_.size(), last);
}

}  // namespace ghostdrive::daemon
