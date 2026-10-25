#include "ghostdrive/daemon/membership.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace ghostdrive::daemon {
namespace {

DriveId drive_id(std::byte fill) {
    DriveId id{};
    id.fill(fill);
    return id;
}

// One device with its own copy of the membership.
struct Device {
    explicit Device(const DriveId& drive, std::string device_name)
        : key(core::DeviceKey::generate().value()),
          name(std::move(device_name)),
          membership(drive, key.public_key()) {}

    const core::PublicKey& id() const { return key.public_key(); }

    // Takes everything `from` has, as the daemon does over a connection, then countersigns.
    void sync_from(const Device& from) {
        auto entries = from.membership.entries();
        membership.merge(entries);
        ASSERT_TRUE(membership.countersign(key));
    }

    bool has(const Device& other) const { return membership.view().members.contains(other.id()); }

    core::DeviceKey key;
    std::string name;
    Membership membership;
};

// The first link of pairing: the inviter adds the new device, which adds the inviter and then
// takes the inviter's entries.
void pair(Device& inviter, Device& joiner) {
    ASSERT_TRUE(inviter.membership.add(inviter.key, joiner.id(), joiner.name, true));
    ASSERT_TRUE(joiner.membership.add(joiner.key, inviter.id(), inviter.name, false));
    joiner.sync_from(inviter);
}

void sync_all(std::vector<Device*> devices) {
    for (int round = 0; round < 3; ++round) {
        for (auto* to : devices) {
            for (auto* from : devices) {
                if (to != from) to->sync_from(*from);
            }
        }
    }
}

TEST(Membership, PairingMakesBothSidesMembers) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    pair(a, b);
    a.sync_from(b);
    EXPECT_TRUE(a.has(b));
    EXPECT_TRUE(b.has(a));
    EXPECT_EQ(b.membership.view().members.at(a.id()), "desk");
    EXPECT_TRUE(a.membership.view().invited_by.at(b.id()).contains(a.id()));
}

TEST(Membership, ADeviceAddedThroughOneMemberReachesTheOthers) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    Device c(id, "phone");
    pair(a, b);
    a.sync_from(b);
    pair(a, c);
    EXPECT_FALSE(b.has(c));
    b.sync_from(a);
    EXPECT_TRUE(b.has(c));
    c.sync_from(b);
    EXPECT_TRUE(c.has(b));
}

TEST(Membership, EntriesWithABadSignatureOrFromAnotherDriveAreDropped) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    pair(a, b);
    Device c(id, "phone");
    ASSERT_TRUE(a.membership.add(a.key, c.id(), "phone", true));
    auto entries = a.membership.entries();

    auto forged = entries;
    for (auto& e : forged) e.name = "renamed";
    EXPECT_EQ(b.membership.merge(forged), 0u);
    EXPECT_FALSE(b.has(c));

    Device other_drive_device(drive_id(std::byte{2}), "x");
    EXPECT_EQ(other_drive_device.membership.merge(entries), 0u);

    EXPECT_GT(b.membership.merge(entries), 0u);
    EXPECT_TRUE(b.has(c));
}

TEST(Membership, AnEntryNeedsItsAuthorsAddInTheSameBatch) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    Device c(id, "phone");
    Device d(id, "tablet");
    pair(a, b);
    pair(a, c);
    ASSERT_TRUE(c.membership.add(c.key, d.id(), "tablet", true));

    // b hears of d from c before it hears of c from a: dropped, not kept.
    auto from_c = c.membership.entries();
    std::vector<MembershipEntry> by_c;
    std::copy_if(from_c.begin(), from_c.end(), std::back_inserter(by_c),
                 [&](const MembershipEntry& e) { return e.author == c.id(); });
    EXPECT_EQ(b.membership.merge(by_c), 0u);
    EXPECT_FALSE(b.has(d));
    // Within one batch the order does not matter.
    auto batch = by_c;
    auto from_a = a.membership.entries();
    batch.insert(batch.end(), from_a.begin(), from_a.end());
    b.membership.merge(batch);
    EXPECT_TRUE(b.has(c));
    EXPECT_TRUE(b.has(d));
}

TEST(Membership, TheViewDoesNotDependOnTheOrderEntriesArrive) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    Device c(id, "phone");
    Device d(id, "tablet");
    pair(a, b);
    pair(b, c);
    pair(a, d);
    sync_all({&a, &b, &c, &d});
    ASSERT_TRUE(b.membership.remove(b.key, d.id()));
    sync_all({&a, &b, &c});

    auto all = a.membership.entries();
    std::mt19937 rng(7);
    std::optional<MembershipView> first;
    for (int i = 0; i < 20; ++i) {
        std::shuffle(all.begin(), all.end(), rng);
        // The view of a, recomputed from shuffled entries.
        Membership m(id, a.id());
        m.merge(all);
        auto view = m.view();
        if (!first) {
            first = view;
            continue;
        }
        EXPECT_EQ(view.members, first->members);
        EXPECT_EQ(view.removed, first->removed);
    }
    ASSERT_TRUE(first);
    EXPECT_FALSE(first->members.contains(d.id()));
    EXPECT_TRUE(first->members.contains(c.id()));
}

TEST(Membership, RemovalIsPermanent) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    Device c(id, "phone");
    pair(a, b);
    pair(a, c);
    sync_all({&a, &b, &c});
    ASSERT_TRUE(a.membership.remove(a.key, c.id()));
    EXPECT_FALSE(a.has(c));
    EXPECT_TRUE(a.membership.view().removed.contains(c.id()));
    b.sync_from(a);
    EXPECT_FALSE(b.has(c));
    // b already vouched for c, and could vouch again; the removal still wins.
    ASSERT_TRUE(b.membership.add(b.key, c.id(), "phone", true));
    EXPECT_FALSE(b.has(c));
    a.sync_from(b);
    EXPECT_FALSE(a.has(c));
}

TEST(Membership, MembersStayWhenTheDeviceThatInvitedThemIsRemoved) {
    auto id = drive_id(std::byte{1});
    Device x(id, "old-laptop");
    Device l(id, "desk");
    Device r(id, "phone");
    pair(x, l);
    pair(x, r);
    sync_all({&x, &l, &r});
    ASSERT_TRUE(l.membership.remove(l.key, x.id()));
    r.sync_from(l);
    EXPECT_FALSE(r.has(x));
    EXPECT_TRUE(r.has(l));
    EXPECT_TRUE(l.has(r));
}

TEST(Membership, DevicesARemovedDeviceAddsAfterwardsAreNotMembers) {
    auto id = drive_id(std::byte{1});
    Device x(id, "stolen");
    Device l(id, "desk");
    Device r(id, "phone");
    pair(x, l);
    pair(x, r);
    sync_all({&x, &l, &r});
    ASSERT_TRUE(l.membership.remove(l.key, x.id()));
    r.sync_from(l);

    Device puppet(id, "puppet");
    ASSERT_TRUE(x.membership.add(x.key, puppet.id(), "puppet", true));
    r.sync_from(x);
    l.sync_from(x);
    EXPECT_FALSE(r.has(puppet));
    EXPECT_FALSE(l.has(puppet));
}

TEST(Membership, ARemovedDeviceCannotActThroughANewKey) {
    auto id = drive_id(std::byte{1});
    Device r(id, "stolen");
    Device d(id, "desk");
    Device h(id, "phone");
    pair(r, d);
    pair(r, h);
    sync_all({&r, &d, &h});
    ASSERT_TRUE(d.membership.remove(d.key, r.id()));
    h.sync_from(d);

    // r makes a key nobody has seen and has it remove the device that removed r.
    Device k(id, "puppet");
    ASSERT_TRUE(r.membership.add(r.key, k.id(), "puppet", true));
    ASSERT_TRUE(k.membership.add(k.key, r.id(), "stolen", false));
    k.membership.merge(r.membership.entries());
    ASSERT_TRUE(k.membership.remove(k.key, d.id()));
    ASSERT_TRUE(k.membership.remove(k.key, h.id()));
    auto attack = r.membership.entries();
    auto more = k.membership.entries();
    attack.insert(attack.end(), more.begin(), more.end());
    d.membership.merge(attack);
    h.membership.merge(attack);

    EXPECT_FALSE(d.has(r));
    EXPECT_FALSE(d.has(k));
    EXPECT_TRUE(d.has(h));
    EXPECT_FALSE(h.has(r));
    EXPECT_FALSE(h.has(k));
}

TEST(Membership, ARemovedDeviceFightingBackChangesNothing) {
    auto id = drive_id(std::byte{1});
    Device x(id, "stolen");
    Device l(id, "desk");
    Device r(id, "phone");
    pair(x, l);
    pair(x, r);
    sync_all({&x, &l, &r});
    ASSERT_TRUE(l.membership.remove(l.key, x.id()));
    r.sync_from(l);
    EXPECT_FALSE(r.has(x));

    // r recorded x as removed and signed its own remove, so x fighting back changes nothing.
    ASSERT_TRUE(x.membership.remove(x.key, l.id()));
    ASSERT_TRUE(x.membership.remove(x.key, r.id()));
    r.sync_from(x);
    l.sync_from(x);
    EXPECT_FALSE(r.has(x));
    EXPECT_TRUE(r.has(l));
    EXPECT_TRUE(l.has(r));
    EXPECT_FALSE(l.has(x));
}

TEST(Membership, OneMemberCannotFillTheDriveSoOthersCannotRemoveIt) {
    auto id = drive_id(std::byte{1});
    Device m(id, "mallory");
    Device h(id, "desk");
    pair(h, m);
    m.sync_from(h);
    for (std::size_t i = 0; i < kMaxDevices + 10; ++i) {
        auto sybil = core::DeviceKey::generate();
        ASSERT_TRUE(sybil);
        (void)m.membership.add(m.key, sybil.value().public_key(), "sybil", false);
    }
    h.membership.merge(m.membership.entries());
    EXPECT_LE(h.membership.view().members.size(), kMaxDevices - 1);
    ASSERT_TRUE(h.membership.remove(h.key, m.id()));
    auto view = h.membership.view();
    EXPECT_FALSE(view.members.contains(m.id()));
    // Every device only m vouched for goes with it.
    EXPECT_TRUE(view.members.empty());
}

TEST(Membership, TheFirstEntryForAStatementWins) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    Device c(id, "phone");
    pair(a, b);
    pair(a, c);
    b.sync_from(a);
    // a signs a second, different add for c.
    MembershipEntry e;
    e.kind = MembershipEntry::Kind::add;
    e.author = a.id();
    e.subject = c.id();
    e.name = "renamed";
    e.signature = a.key.sign(membership_signed_bytes(id, e)).value();
    auto before = b.membership.last_sequence();
    EXPECT_EQ(b.membership.merge(std::span(&e, 1)), 0u);
    EXPECT_EQ(b.membership.last_sequence(), before);
    EXPECT_EQ(b.membership.view().members.at(c.id()), "phone");
}

TEST(Membership, CountersigningWaitsUntilAMemberIsReady) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    Device c(id, "phone");
    pair(a, b);
    pair(a, c);
    b.membership.merge(a.membership.entries());
    auto none = b.membership.countersign(b.key, [](const core::PublicKey&, bool) { return false; });
    ASSERT_TRUE(none);
    EXPECT_EQ(none.value(), 0u);
    auto some = b.membership.countersign(b.key, [&](const core::PublicKey& k, bool) { return k == c.id(); });
    ASSERT_TRUE(some);
    EXPECT_EQ(some.value(), 1u);
    EXPECT_TRUE(b.membership.view().vouched_by.at(c.id()).contains(b.id()));
}

TEST(Membership, AContestNobodySettledRemovesBothSides) {
    auto id = drive_id(std::byte{1});
    Device x(id, "stolen");
    Device l(id, "desk");
    Device r(id, "phone");
    pair(x, l);
    pair(x, r);
    sync_all({&x, &l, &r});
    // Both removals reach r together, before r took a side.
    ASSERT_TRUE(l.membership.remove(l.key, x.id()));
    ASSERT_TRUE(x.membership.remove(x.key, l.id()));
    auto entries = l.membership.entries();
    auto more = x.membership.entries();
    entries.insert(entries.end(), more.begin(), more.end());
    r.membership.merge(entries);
    auto view = r.membership.view();
    EXPECT_FALSE(view.members.contains(x.id()));
    EXPECT_FALSE(view.members.contains(l.id()));
    // l trusts its own judgement; removing x on r as well settles it there too.
    EXPECT_TRUE(l.has(r));
    ASSERT_TRUE(r.membership.remove(r.key, x.id()));
    EXPECT_TRUE(r.has(l));
}

TEST(Membership, RemovalsOfThisDeviceNeverCountHere) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    pair(a, b);
    a.sync_from(b);
    ASSERT_TRUE(a.membership.remove(a.key, b.id()));
    b.sync_from(a);
    auto view = b.membership.view();
    ASSERT_TRUE(view.removed_by);
    EXPECT_EQ(*view.removed_by, a.id());
    EXPECT_TRUE(view.members.contains(a.id()));
    EXPECT_FALSE(b.membership.remove(b.key, b.id()));
    EXPECT_FALSE(b.membership.add(b.key, b.id(), "laptop", false));
}

TEST(Membership, SerializeRoundTripsAndRejectsTampering) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    pair(a, b);
    a.sync_from(b);
    auto bytes = a.membership.serialize();
    auto parsed = Membership::parse(bytes, id, a.id());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().entries(), a.membership.entries());
    EXPECT_EQ(parsed.value().view().members, a.membership.view().members);

    auto tampered = bytes;
    tampered.back() ^= std::byte{1};
    EXPECT_FALSE(Membership::parse(tampered, id, a.id()));
    EXPECT_FALSE(Membership::parse(bytes, drive_id(std::byte{3}), a.id()));
}

TEST(Membership, SinceReturnsOnlyNewerEntries) {
    auto id = drive_id(std::byte{1});
    Device a(id, "desk");
    Device b(id, "laptop");
    Device c(id, "phone");
    ASSERT_TRUE(a.membership.add(a.key, b.id(), "laptop", true));
    std::uint64_t last = 0;
    auto first = a.membership.since(0, 100, last);
    ASSERT_EQ(first.size(), 1u);
    ASSERT_TRUE(a.membership.add(a.key, c.id(), "phone", true));
    auto second = a.membership.since(last, 100, last);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].subject, c.id());
    EXPECT_TRUE(a.membership.since(last, 100, last).empty());
    EXPECT_EQ(last, a.membership.last_sequence());
}

TEST(Membership, ALoopOfKeysVouchingForEachOtherIsOut) {
    auto id = drive_id(std::byte{1});
    Device r(id, "stolen");
    Device d(id, "desk");
    Device late(id, "laptop");
    pair(r, d);
    pair(r, late);
    sync_all({&r, &d, &late});

    // r's new keys vouch for each other and remove d.
    Device k(id, "k");
    Device k2(id, "k2");
    ASSERT_TRUE(r.membership.add(r.key, k.id(), "k", true));
    ASSERT_TRUE(k.membership.add(k.key, r.id(), "stolen", false));
    k.membership.merge(r.membership.entries());
    ASSERT_TRUE(k.membership.add(k.key, k2.id(), "k2", true));
    ASSERT_TRUE(k2.membership.add(k2.key, k.id(), "k", true));
    k2.membership.merge(k.membership.entries());
    k.membership.merge(k2.membership.entries());
    ASSERT_TRUE(k.membership.remove(k.key, d.id()));
    auto attack = r.membership.entries();
    auto more = k.membership.entries();
    attack.insert(attack.end(), more.begin(), more.end());

    // late stores it all while r still looks fine (no vouching yet), then removes r. Without
    // r, nothing this device trusts leads to k or k2, so k's remove of d stops counting.
    late.membership.merge(attack);
    auto stored = late.membership.entries();
    ASSERT_TRUE(std::any_of(stored.begin(), stored.end(), [&](const MembershipEntry& e) {
        return e.kind == MembershipEntry::Kind::remove && e.author == k.id() && e.subject == d.id();
    })) << "the attack was not stored, so this test proves nothing";
    EXPECT_FALSE(late.has(d));
    ASSERT_TRUE(late.membership.remove(late.key, r.id()));
    EXPECT_TRUE(late.has(d));
    EXPECT_FALSE(late.has(k));
    EXPECT_FALSE(late.has(k2));

    // d, which removed r first, never even stores the keys' entries.
    ASSERT_TRUE(d.membership.remove(d.key, r.id()));
    EXPECT_EQ(d.membership.merge(more), 0u);
    EXPECT_TRUE(d.has(late));
}

TEST(Membership, RemovingTheRemoverDoesNotBringTheRemovedBack) {
    auto id = drive_id(std::byte{1});
    Device r(id, "stolen");
    Device d(id, "old-laptop");
    Device x(id, "desk");
    pair(x, r);
    pair(x, d);
    sync_all({&r, &d, &x});
    ASSERT_TRUE(d.membership.remove(d.key, r.id()));
    x.sync_from(d);
    EXPECT_FALSE(x.has(r));
    ASSERT_TRUE(x.membership.remove(x.key, d.id()));
    EXPECT_FALSE(x.has(d));
    EXPECT_FALSE(x.has(r));
    EXPECT_TRUE(x.membership.view().removed.contains(r.id()));

    // A device that was offline throughout hears of both removals at once.
    Device y(id, "phone");
    ASSERT_TRUE(x.membership.add(x.key, y.id(), "phone", true));
    ASSERT_TRUE(y.membership.add(y.key, x.id(), "desk", false));
    y.sync_from(x);
    EXPECT_FALSE(y.has(r));
    EXPECT_FALSE(y.has(d));

    // And the record survives a restart.
    auto reloaded = Membership::parse(x.membership.serialize(), id, x.id());
    ASSERT_TRUE(reloaded);
    EXPECT_TRUE(reloaded.value().view().removed.contains(r.id()));
}

TEST(Membership, SlotsTakenByARemovedMembersDevicesAreFreed) {
    auto id = drive_id(std::byte{1});
    Device m(id, "mallory");
    Device h(id, "desk");
    pair(h, m);
    m.sync_from(h);
    for (std::size_t i = 0; i < kMaxDevices; ++i) {
        auto sybil = core::DeviceKey::generate();
        ASSERT_TRUE(sybil);
        (void)m.membership.add(m.key, sybil.value().public_key(), "sybil", false);
    }
    h.membership.merge(m.membership.entries());
    Device newcomer(id, "phone");
    EXPECT_FALSE(h.membership.add(h.key, newcomer.id(), "phone", true)) << "the drive is full";
    ASSERT_TRUE(h.membership.remove(h.key, m.id()));
    EXPECT_TRUE(h.membership.add(h.key, newcomer.id(), "phone", true));
    EXPECT_TRUE(h.has(newcomer));
}

TEST(Membership, AnyKnownDeviceCanBeRemovedEvenInDoubt) {
    auto id = drive_id(std::byte{1});
    Device x(id, "stolen");
    Device l(id, "desk");
    Device r(id, "phone");
    pair(x, l);
    pair(x, r);
    sync_all({&x, &l, &r});
    ASSERT_TRUE(l.membership.remove(l.key, x.id()));
    ASSERT_TRUE(x.membership.remove(x.key, l.id()));
    auto entries = l.membership.entries();
    auto more = x.membership.entries();
    entries.insert(entries.end(), more.begin(), more.end());
    r.membership.merge(entries);
    auto view = r.membership.view();
    EXPECT_TRUE(view.in_doubt.contains(x.id()));
    EXPECT_TRUE(view.in_doubt.contains(l.id()));
    EXPECT_EQ(view.names.at(x.id()), "stolen");
    ASSERT_TRUE(r.membership.remove(r.key, x.id()));
    EXPECT_TRUE(r.has(l));
}

TEST(Membership, ARemoveFromADeviceRemovedElsewhereDoesNotBecomePermanentAtOnce) {
    auto id = drive_id(std::byte{1});
    Device r(id, "stolen");
    Device d(id, "desk");
    Device z(id, "old-laptop");
    Device h(id, "phone");
    pair(r, d);
    pair(r, z);
    pair(r, h);
    sync_all({&r, &d, &z, &h});
    // d removes r while z is offline; r reaches z first and removes h there.
    ASSERT_TRUE(d.membership.remove(d.key, r.id()));
    ASSERT_TRUE(r.membership.remove(r.key, h.id()));
    z.membership.merge(r.membership.entries());
    auto not_yet = [](const core::PublicKey&, bool removal) { return !removal; };
    ASSERT_TRUE(z.membership.countersign(z.key, not_yet));
    EXPECT_FALSE(z.has(h));
    EXPECT_TRUE(z.membership.view().removal_pending.contains(h.id()));
    // Then z hears of r's removal, and h is back.
    z.membership.merge(d.membership.entries());
    ASSERT_TRUE(z.membership.countersign(z.key, not_yet));
    EXPECT_TRUE(z.has(h));
    EXPECT_FALSE(z.has(r));
    for (const auto& e : z.membership.entries()) {
        EXPECT_FALSE(e.kind == MembershipEntry::Kind::remove && e.author == z.id() &&
                     e.subject == h.id());
    }
}

TEST(Membership, ReaddingDevicesThatAreOutCountsAgainstTheLiveLimit) {
    auto id = drive_id(std::byte{1});
    Device h(id, "desk");
    Device m(id, "mallory");
    Device a(id, "a");
    pair(h, m);
    m.sync_from(h);
    ASSERT_TRUE(m.membership.add(m.key, a.id(), "a", true));
    ASSERT_TRUE(a.membership.add(a.key, m.id(), "mallory", false));
    a.membership.merge(m.membership.entries());
    std::vector<core::PublicKey> keys;
    for (std::size_t i = 0; i + 3 < kMaxDevices; ++i) {
        keys.push_back(core::DeviceKey::generate().value().public_key());
        ASSERT_TRUE(a.membership.add(a.key, keys.back(), "k", true));
    }
    h.membership.merge(m.membership.entries());
    h.membership.merge(a.membership.entries());
    EXPECT_EQ(h.membership.view().members.size(), kMaxDevices - 1);

    // m removes a, which leaves a's keys unanchored and frees their slots, then vouches for
    // all of them itself and for more new keys.
    // The removal goes first, so the slots are free when the vouches arrive. Those are signed
    // directly, as an attacker would, not through m's own (limited) Membership.
    ASSERT_TRUE(m.membership.remove(m.key, a.id()));
    h.membership.merge(m.membership.entries());
    ASSERT_FALSE(h.has(a));
    std::vector<MembershipEntry> attack;
    auto vouch = [&](const core::PublicKey& subject) {
        MembershipEntry e;
        e.kind = MembershipEntry::Kind::add;
        e.author = m.id();
        e.subject = subject;
        e.name = "k";
        e.signature = m.key.sign(membership_signed_bytes(id, e)).value();
        attack.push_back(e);
    };
    for (const auto& k : keys) vouch(k);
    for (int i = 0; i < 10; ++i) vouch(core::DeviceKey::generate().value().public_key());
    h.membership.merge(attack);
    auto view = h.membership.view();
    EXPECT_LE(view.members.size() + view.in_doubt.size() + 1, kMaxDevices);
}

// Keys from a counter, so the many keys the compaction tests need are the same on every run.
core::DeviceKey seeded_key(std::uint32_t n) {
    std::array<std::byte, core::kSeedSize> seed{};
    for (std::size_t i = 0; i < 4; ++i) seed[i] = static_cast<std::byte>(n >> (8 * i));
    return core::DeviceKey::from_seed(seed).value();
}

// Signed directly rather than through the author's own Membership, which would settle its
// whole log for every entry.
MembershipEntry signed_entry(const DriveId& drive, const core::DeviceKey& author,
                             MembershipEntry::Kind kind, const core::PublicKey& subject) {
    MembershipEntry e;
    e.kind = kind;
    e.author = author.public_key();
    e.subject = subject;
    if (kind == MembershipEntry::Kind::add) e.name = "k";
    e.signature = author.sign(membership_signed_bytes(drive, e)).value();
    return e;
}

std::size_t own_entries(const Device& d) {
    auto all = d.membership.entries();
    return static_cast<std::size_t>(std::count_if(
        all.begin(), all.end(), [&](const MembershipEntry& e) { return e.author == d.id(); }));
}

// With h, m and the voucher, a wave takes every device slot.
constexpr std::size_t kWaveSize = kMaxDevices - 3;

// A voucher's devices: the voucher and m vouch, and each device vouches for everyone else in
// the wave and removes the other devices, so each signs close to kMaxEntriesPerAuthor entries.
// Each device's entries are signed on one of several threads; the result is the same.
std::vector<MembershipEntry> wave_entries(const DriveId& drive, const core::DeviceKey& voucher,
                                          const std::vector<core::DeviceKey>& devices,
                                          const Device& h, const Device& m) {
    using Kind = MembershipEntry::Kind;
    std::vector<MembershipEntry> out;
    for (const auto& d : devices)
        out.push_back(signed_entry(drive, voucher, Kind::add, d.public_key()));
    out.push_back(signed_entry(drive, m.key, Kind::add, voucher.public_key()));
    const std::size_t head = out.size();
    const std::size_t per_device = 3 + 2 * (devices.size() - 1);
    out.resize(head + devices.size() * per_device);
    auto sign_device = [&](std::size_t i) {
        const auto& d = devices[i];
        auto* next = &out[head + i * per_device];
        *next++ = signed_entry(drive, d, Kind::add, voucher.public_key());
        *next++ = signed_entry(drive, d, Kind::add, h.id());
        *next++ = signed_entry(drive, d, Kind::add, m.id());
        for (const auto& other : devices) {
            if (&other == &d) continue;
            *next++ = signed_entry(drive, d, Kind::add, other.public_key());
            *next++ = signed_entry(drive, d, Kind::remove, other.public_key());
        }
    };
    const std::size_t threads = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (std::size_t i = t; i < devices.size(); i += threads) sign_device(i);
        });
    }
    for (auto& w : workers) w.join();
    return out;
}

// Fills h's log up to the limit for other authors' entries, without compacting. Waves of
// devices come in until the log is full; h removes every wave but the last for good, so all
// their entries (and m's vouches for their vouchers) no longer count. Those devices are `gone`.
void fill_log(const DriveId& drive, Device& h, Device& m, std::set<core::PublicKey>& gone) {
    pair(h, m);
    h.sync_from(m);
    const std::size_t room = kMaxMembershipEntries - kMaxOwnEntries;
    std::uint32_t seed = 1;
    for (;;) {
        auto voucher = seeded_key(seed++);
        std::vector<core::DeviceKey> devices;
        for (std::size_t i = 0; i < kWaveSize; ++i) devices.push_back(seeded_key(seed++));
        ASSERT_TRUE(h.membership.add(h.key, voucher.public_key(), "voucher", true));
        auto entries = wave_entries(drive, voucher, devices, h, m);
        auto left = room - h.membership.entries().size();
        bool last = entries.size() >= left;
        if (last) entries.resize(left);
        ASSERT_EQ(h.membership.merge(entries), entries.size());
        if (last) return;
        std::vector<MembershipEntry> removes;
        for (const auto& d : devices) {
            removes.push_back(
                signed_entry(drive, h.key, MembershipEntry::Kind::remove, d.public_key()));
            gone.insert(d.public_key());
        }
        removes.push_back(
            signed_entry(drive, h.key, MembershipEntry::Kind::remove, voucher.public_key()));
        gone.insert(voucher.public_key());
        ASSERT_EQ(h.membership.merge(removes), removes.size());
        ASSERT_TRUE(h.membership.countersign(h.key));
    }
}

TEST(Membership, AFullLogIsCompactedAndTakesEntriesAgain) {
    auto id = drive_id(std::byte{1});
    Device h(id, "desk");
    Device m(id, "laptop");
    std::set<core::PublicKey> gone;
    fill_log(id, h, m, gone);
    ASSERT_FALSE(HasFatalFailure());
    auto full = h.membership.entries().size();
    ASSERT_EQ(full, kMaxMembershipEntries - kMaxOwnEntries);
    auto before = h.membership.view();

    // A remove of this device never counts here, so this entry by m changes neither list.
    auto e = signed_entry(id, m.key, MembershipEntry::Kind::remove, h.id());
    EXPECT_EQ(h.membership.merge(std::span(&e, 1)), 1u);
    auto stored = h.membership.entries();
    EXPECT_LT(stored.size(), full / 2);
    EXPECT_EQ(stored.back(), e);
    auto after = h.membership.view();
    EXPECT_EQ(after.members, before.members);
    EXPECT_EQ(after.removed, before.removed);
    EXPECT_EQ(after.in_doubt, before.in_doubt);
    EXPECT_TRUE(after.members.contains(m.id()));

    // And entries keep coming in, from m and from h itself. (Every device slot is taken, so
    // they are about a device already known.)
    ASSERT_FALSE(before.in_doubt.empty());
    auto doubted = *before.in_doubt.begin();
    auto vouch = signed_entry(id, m.key, MembershipEntry::Kind::add, doubted);
    EXPECT_EQ(h.membership.merge(std::span(&vouch, 1)), 1u);
    EXPECT_TRUE(h.membership.add(h.key, doubted, "k", false));
    EXPECT_EQ(h.membership.entries().size(), stored.size() + 2);
}

TEST(Membership, CompactionDropsEntriesThatNoLongerCount) {
    auto id = drive_id(std::byte{1});
    Device h(id, "desk");
    Device m(id, "laptop");
    std::set<core::PublicKey> gone;
    fill_log(id, h, m, gone);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_FALSE(gone.empty());
    auto e = signed_entry(id, m.key, MembershipEntry::Kind::remove, h.id());
    ASSERT_EQ(h.membership.merge(std::span(&e, 1)), 1u);

    // Counted rather than checked one by one: a broken compaction leaves tens of thousands.
    std::size_t by_gone = 0, about_gone = 0, own_adds_of_gone = 0, own_removes_of_gone = 0;
    for (const auto& s : h.membership.entries()) {
        if (s.author == h.id()) {
            if (!gone.contains(s.subject)) continue;
            if (s.kind == MembershipEntry::Kind::add) ++own_adds_of_gone;
            if (s.kind == MembershipEntry::Kind::remove) ++own_removes_of_gone;
            continue;
        }
        if (gone.contains(s.author)) ++by_gone;
        if (gone.contains(s.subject)) ++about_gone;
    }
    EXPECT_EQ(by_gone, 0u);
    EXPECT_EQ(about_gone, 0u);
    EXPECT_EQ(own_adds_of_gone, 0u);
    // Fewer than kMaxKnownKeys, so this device's removes all stay to tell the others.
    ASSERT_LE(gone.size(), kMaxKnownKeys);
    EXPECT_EQ(own_removes_of_gone, gone.size());

    auto parsed = Membership::parse(h.membership.serialize(), id, h.id());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().entries(), h.membership.entries());
    EXPECT_EQ(parsed.value().view().members, h.membership.view().members);
    EXPECT_EQ(parsed.value().view().removed, h.membership.view().removed);
}

TEST(Membership, CompactionKeepsOnlyTheNewestOwnRemoves) {
    auto id = drive_id(std::byte{1});
    Device h(id, "desk");
    std::vector<core::PublicKey> removed;  // in the order h removed them
    std::uint32_t seed = 1;
    // Each round h adds a voucher, which vouches for as many devices as fit, and h removes them
    // all for good: one add and kMaxDevices - 1 removes, until h's own entries are full.
    for (std::size_t round = 0; round < kMaxOwnEntries / kMaxDevices; ++round) {
        auto voucher = seeded_key(seed++);
        ASSERT_TRUE(h.membership.add(h.key, voucher.public_key(), "voucher", true));
        std::vector<MembershipEntry> adds;
        for (std::size_t i = 0; i < kMaxDevices - 2; ++i) {
            adds.push_back(signed_entry(id, voucher, MembershipEntry::Kind::add,
                                        seeded_key(seed++).public_key()));
        }
        ASSERT_EQ(h.membership.merge(adds), adds.size());
        for (const auto& a : adds) {
            ASSERT_TRUE(h.membership.remove(h.key, a.subject));
            removed.push_back(a.subject);
        }
        ASSERT_TRUE(h.membership.remove(h.key, voucher.public_key()));
        removed.push_back(voucher.public_key());
        ASSERT_TRUE(h.membership.countersign(h.key));
    }
    ASSERT_EQ(own_entries(h), kMaxOwnEntries);
    ASSERT_GT(removed.size(), kMaxKnownKeys);

    Device newcomer(id, "phone");
    ASSERT_TRUE(h.membership.add(h.key, newcomer.id(), "phone", true));
    std::vector<core::PublicKey> kept;
    for (const auto& e : h.membership.entries()) {
        if (e.author == h.id() && e.kind == MembershipEntry::Kind::remove)
            kept.push_back(e.subject);
    }
    std::vector<core::PublicKey> newest(removed.end() - kMaxKnownKeys, removed.end());
    EXPECT_EQ(kept, newest);
    EXPECT_TRUE(h.has(newcomer));
    for (const auto& key : removed) EXPECT_TRUE(h.membership.view().removed.contains(key));
}

TEST(Membership, AFullLogThatFreesNothingIsNotCompactedOnEveryTry) {
    auto id = drive_id(std::byte{1});
    Device h(id, "desk");
    std::uint32_t seed = 1;
    // h's own entries, none of which compaction may drop: kMaxKnownKeys removes of devices
    // removed for good (their vouchers stay members), then adds and removes of devices that
    // are not removed for good, since h never countersigns those.
    constexpr std::size_t kRounds = 8;
    for (std::size_t round = 0; round < kRounds; ++round) {
        auto voucher = seeded_key(seed++);
        ASSERT_TRUE(h.membership.add(h.key, voucher.public_key(), "voucher", true));
        std::vector<MembershipEntry> adds;
        for (std::size_t i = 0; i < kMaxKnownKeys / kRounds; ++i) {
            adds.push_back(signed_entry(id, voucher, MembershipEntry::Kind::add,
                                        seeded_key(seed++).public_key()));
        }
        ASSERT_EQ(h.membership.merge(adds), adds.size());
        for (const auto& a : adds) ASSERT_TRUE(h.membership.remove(h.key, a.subject));
        ASSERT_TRUE(h.membership.countersign(h.key));
    }
    while (own_entries(h) < kMaxOwnEntries) {
        std::vector<core::PublicKey> batch;
        for (std::size_t i = 0; i < 31 && own_entries(h) < kMaxOwnEntries; ++i) {
            batch.push_back(seeded_key(seed++).public_key());
            ASSERT_TRUE(h.membership.add(h.key, batch.back(), "k", false));
        }
        for (const auto& key : batch) {
            if (own_entries(h) == kMaxOwnEntries) break;
            ASSERT_TRUE(h.membership.remove(h.key, key));
        }
    }
    ASSERT_EQ(own_entries(h), kMaxOwnEntries);
    // This try compacts, frees none of h's own entries, and fails.
    EXPECT_FALSE(h.membership.add(h.key, seeded_key(seed++).public_key(), "k", false));
    ASSERT_EQ(own_entries(h), kMaxOwnEntries);

    // Own entries that cannot be placed, offered twice: the second time they are already
    // verified, so the merge costs one settle (a view) plus cheap lookups, unless every try
    // compacts again, which costs at least a settle each.
    constexpr std::size_t kTries = 2000;
    std::vector<MembershipEntry> extra;
    for (std::size_t i = 0; i < kTries; ++i) {
        extra.push_back(
            signed_entry(id, h.key, MembershipEntry::Kind::add, seeded_key(seed++).public_key()));
    }
    ASSERT_EQ(h.membership.merge(extra), 0u);
    using Clock = std::chrono::steady_clock;
    std::vector<Clock::duration> views;
    for (int i = 0; i < 5; ++i) {
        auto start = Clock::now();
        (void)h.membership.view();
        views.push_back(Clock::now() - start);
    }
    std::sort(views.begin(), views.end());
    auto start = Clock::now();
    EXPECT_EQ(h.membership.merge(extra), 0u);
    auto merge = Clock::now() - start;
    EXPECT_LT(merge, views[views.size() / 2] * (kTries / 10))
        << "merge took " << std::chrono::duration<double, std::milli>(merge).count()
        << " ms, a view "
        << std::chrono::duration<double, std::milli>(views[views.size() / 2]).count() << " ms";
}

}  // namespace
}  // namespace ghostdrive::daemon
