#include "ghostdrive/daemon/discovery_keys.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace ghostdrive::daemon {
namespace {

DriveId drive_id() {
    DriveId id{};
    id.fill(std::byte{7});
    return id;
}

core::DriveSecret drive_secret() {
    core::DriveSecret secret{};
    secret.fill(std::byte{5});
    return secret;
}

struct Device {
    Device() : key(core::DeviceKey::generate().value()), keys(drive_id(), id(), drive_secret()) {}

    const core::PublicKey& id() const { return key.public_key(); }

    core::DeviceKey key;
    DiscoveryKeys keys;
};

bool holds(const DiscoveryKeys& keys, const core::DriveSecret& secret) {
    auto all = keys.all();
    return std::any_of(all.begin(), all.end(), [&](const auto& k) { return *k.key == secret; });
}

TEST(DiscoveryKeys, StartWithTheDriveSecret) {
    Device a;
    EXPECT_EQ(a.keys.current(), drive_secret());
    EXPECT_EQ(a.keys.current_id().first, 0u);
    EXPECT_TRUE(a.keys.keys().empty()) << "the drive secret is never sent";
    auto nothing = a.keys.update(a.key, {}, {});
    ASSERT_TRUE(nothing);
    EXPECT_FALSE(nothing.value()) << "nobody removed, nothing to replace";
}

TEST(DiscoveryKeys, RemovingADeviceMakesANewKeyOnce) {
    Device a;
    core::PublicKey removed{};
    removed.fill(std::byte{1});
    auto made = a.keys.update(a.key, {}, {removed});
    ASSERT_TRUE(made);
    EXPECT_TRUE(made.value());
    EXPECT_NE(a.keys.current(), drive_secret());
    EXPECT_EQ(a.keys.current_id(), std::pair(std::uint64_t{1}, a.id()));
    auto first = a.keys.current();

    auto again = a.keys.update(a.key, {}, {removed});
    ASSERT_TRUE(again);
    EXPECT_FALSE(again.value());
    EXPECT_EQ(a.keys.current(), first);
    // The drive secret is kept for matching devices that have not caught up.
    EXPECT_TRUE(holds(a.keys, drive_secret()));
}

TEST(DiscoveryKeys, TheHighestGenerationFromAMemberWins) {
    Device a;
    Device b;
    core::PublicKey r{};
    r.fill(std::byte{1});
    ASSERT_TRUE(a.keys.update(a.key, {b.id()}, {r}));
    ASSERT_TRUE(b.keys.update(b.key, {a.id()}, {r}));
    // Both made generation 1; the larger author breaks the tie, the same way on both.
    EXPECT_TRUE(a.keys.merge(b.keys.keys(), {b.id()}));
    EXPECT_TRUE(b.keys.merge(a.keys.keys(), {a.id()}));
    EXPECT_EQ(a.keys.current(), b.keys.current());
    EXPECT_EQ(a.keys.current_id().second, std::max(a.id(), b.id()));

    // A key one generation up replaces both.
    core::PublicKey r2{};
    r2.fill(std::byte{2});
    ASSERT_TRUE(b.keys.update(b.key, {a.id()}, {r, r2}));
    EXPECT_EQ(b.keys.current_id(), std::pair(std::uint64_t{2}, b.id()));
    EXPECT_TRUE(a.keys.merge(b.keys.keys(), {b.id()}));
    EXPECT_EQ(a.keys.current(), b.keys.current());
    EXPECT_FALSE(a.keys.merge(b.keys.keys(), {b.id()})) << "nothing new the second time";
}

TEST(DiscoveryKeys, KeysFromNonMembersForgeriesAndOlderKeysAreDropped) {
    Device a;
    Device b;
    Device outsider;
    core::PublicKey r{};
    r.fill(std::byte{1});
    ASSERT_TRUE(outsider.keys.update(outsider.key, {}, {r}));
    EXPECT_FALSE(a.keys.merge(outsider.keys.keys(), {b.id()}));
    EXPECT_EQ(a.keys.current(), drive_secret());

    ASSERT_TRUE(b.keys.update(b.key, {}, {r}));
    auto forged = b.keys.keys();
    forged[0].key[0] ^= std::byte{1};
    EXPECT_FALSE(a.keys.merge(forged, {b.id()}));
    auto renamed = b.keys.keys();
    renamed[0].author = a.id();
    EXPECT_FALSE(a.keys.merge(renamed, {b.id()})) << "nobody sends this device its own keys";

    auto older = b.keys.keys();
    core::PublicKey r2{};
    r2.fill(std::byte{2});
    ASSERT_TRUE(b.keys.update(b.key, {}, {r, r2}));
    EXPECT_TRUE(a.keys.merge(b.keys.keys(), {b.id()}));
    EXPECT_FALSE(a.keys.merge(older, {b.id()}));
    EXPECT_EQ(a.keys.current(), b.keys.current());
}

TEST(DiscoveryKeys, AKeyByARemovedDeviceStopsCounting) {
    Device a;
    Device b;
    Device stolen;
    // Before anyone here knows it was removed, the stolen device sends a key far above the rest.
    core::PublicKey earlier{};
    earlier.fill(std::byte{1});
    for (int i = 0; i < 5; ++i) {
        core::PublicKey removed{};
        removed.fill(std::byte(10 + i));
        ASSERT_TRUE(stolen.keys.update(stolen.key, {}, {earlier, removed}));
    }
    ASSERT_TRUE(b.keys.update(b.key, {a.id()}, {earlier}));
    EXPECT_TRUE(a.keys.merge(b.keys.keys(), {b.id(), stolen.id()}));
    EXPECT_TRUE(a.keys.merge(stolen.keys.keys(), {b.id(), stolen.id()}));
    EXPECT_EQ(a.keys.current(), stolen.keys.current());

    // Its removal reaches A: its key no longer counts, and A makes one it never sees.
    auto changed = a.keys.update(a.key, {b.id()}, {earlier, stolen.id()});
    ASSERT_TRUE(changed);
    EXPECT_TRUE(changed.value());
    EXPECT_EQ(a.keys.current_id(), std::pair(std::uint64_t{2}, a.id()));
    // Its key is kept only as an earlier one (a member may still use it), never sent on.
    for (const auto& k : a.keys.keys()) EXPECT_NE(k.author, stolen.id());
    EXPECT_FALSE(holds(stolen.keys, a.keys.current()));
    // B, which never took the stolen device's key, agrees once it hears from A.
    EXPECT_TRUE(b.keys.merge(a.keys.keys(), {a.id()}));
    EXPECT_EQ(b.keys.current(), a.keys.current());
}

TEST(DiscoveryKeys, DevicesThatLearnLateStillEndOnAKeyTheRemovedDeviceNeverSaw) {
    // A learns of R's removal first. B hears A's new key before the removal and passes it on
    // to R, then learns of the removal itself.
    Device a;
    Device b;
    Device r;
    ASSERT_TRUE(a.keys.update(a.key, {b.id()}, {r.id()}));
    ASSERT_TRUE(b.keys.merge(a.keys.keys(), {a.id(), r.id()}));
    ASSERT_TRUE(r.keys.merge(b.keys.keys(), {a.id(), b.id()}));
    EXPECT_EQ(r.keys.current(), a.keys.current()) << "the leak this guards against";

    ASSERT_TRUE(b.keys.update(b.key, {a.id()}, {r.id()}));
    ASSERT_TRUE(a.keys.merge(b.keys.keys(), {b.id()}));
    EXPECT_EQ(a.keys.current(), b.keys.current());
    EXPECT_EQ(a.keys.current_id(), std::pair(std::uint64_t{2}, b.id()));
    EXPECT_FALSE(holds(r.keys, a.keys.current()));
}

TEST(DiscoveryKeys, SurviveSavingAndLoading) {
    Device a;
    Device b;
    core::PublicKey r{};
    r.fill(std::byte{1});
    ASSERT_TRUE(b.keys.update(b.key, {}, {r}));
    ASSERT_TRUE(a.keys.merge(b.keys.keys(), {b.id()}));
    ASSERT_TRUE(a.keys.update(a.key, {b.id()}, {r}));

    auto data = a.keys.serialize();
    auto parsed = DiscoveryKeys::parse(data, drive_id(), a.id(), drive_secret());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().current(), a.keys.current());
    EXPECT_EQ(parsed.value().keys(), a.keys.keys());
    auto same = parsed.value().update(a.key, {b.id()}, {r});
    ASSERT_TRUE(same);
    EXPECT_FALSE(same.value()) << "the removed set it made a key for was saved too";

    auto tampered = data;
    tampered[tampered.size() - 1] ^= std::byte{1};
    EXPECT_FALSE(DiscoveryKeys::parse(tampered, drive_id(), a.id(), drive_secret()));
    DriveId other{};
    EXPECT_FALSE(DiscoveryKeys::parse(data, other, a.id(), drive_secret()))
        << "keys are bound to their drive";
    EXPECT_FALSE(DiscoveryKeys::parse({}, drive_id(), a.id(), drive_secret()));
}

TEST(DiscoveryKeys, AJoinedDeviceDoesNotReplaceTheKeyItWasGiven) {
    Device a;
    Device joined;
    core::PublicKey r{};
    r.fill(std::byte{1});
    ASSERT_TRUE(a.keys.update(a.key, {}, {r}));
    ASSERT_TRUE(joined.keys.merge(a.keys.keys(), {a.id()}));
    joined.keys.set_removed({r});
    auto changed = joined.keys.update(joined.key, {a.id()}, {r});
    ASSERT_TRUE(changed);
    EXPECT_FALSE(changed.value());
    EXPECT_EQ(joined.keys.current(), a.keys.current());
}

TEST(DiscoveryKeys, AKeyFarAboveTheRestIsRefused) {
    // A device about to be removed could otherwise use up the generations, so that nobody can
    // make a key above one it holds.
    Device a;
    Device b;
    DiscoveryKey jump;
    jump.generation = kMaxDiscoveryGenerationStep + 1;
    jump.author = b.id();
    jump.key.fill(std::byte{3});
    jump.signature = b.key.sign(discovery_key_signed_bytes(drive_id(), jump)).value();
    EXPECT_FALSE(a.keys.merge(std::span(&jump, 1), {b.id()}));
    EXPECT_EQ(a.keys.current(), drive_secret());
    jump.generation = kMaxDiscoveryGenerationStep;
    jump.signature = b.key.sign(discovery_key_signed_bytes(drive_id(), jump)).value();
    EXPECT_TRUE(a.keys.merge(std::span(&jump, 1), {b.id()}));
    EXPECT_EQ(a.keys.current(), jump.key);

    // Several keys from one author in one frame climb no further than one.
    std::vector<DiscoveryKey> ladder;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        DiscoveryKey step = jump;
        step.generation = jump.generation + i * kMaxDiscoveryGenerationStep;
        step.key.fill(std::byte(10 + i));
        step.signature = b.key.sign(discovery_key_signed_bytes(drive_id(), step)).value();
        ladder.push_back(step);
    }
    EXPECT_TRUE(a.keys.merge(ladder, {b.id()}));
    EXPECT_EQ(a.keys.current_id().first, jump.generation + kMaxDiscoveryGenerationStep);
}

TEST(DiscoveryKeys, EarlierKeysAreKeptForMembersThatMissedAChange) {
    Device a;
    EXPECT_TRUE(a.keys.earlier().empty()) << "the drive secret is current";
    std::vector<core::DriveSecret> made;
    std::set<core::PublicKey> removed;
    for (int i = 0; i < 5; ++i) {
        core::PublicKey r{};
        r.fill(std::byte(i + 1));
        removed.insert(r);
        ASSERT_TRUE(a.keys.update(a.key, {}, removed));
        made.push_back(a.keys.current());
    }
    auto earlier = a.keys.earlier();
    ASSERT_EQ(earlier.size(), DiscoveryKeys::kMaxEarlierKeys + 1);
    EXPECT_EQ(*earlier[0], made[3]) << "newest first";
    EXPECT_EQ(*earlier[2], made[1]);
    EXPECT_EQ(*earlier.back(), drive_secret());
    EXPECT_TRUE(holds(a.keys, made[2]));
    EXPECT_FALSE(holds(a.keys, made[0])) << "only the last few are kept";

    auto data = a.keys.serialize();
    auto parsed = DiscoveryKeys::parse(data, drive_id(), a.id(), drive_secret());
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto again = parsed.value().earlier();
    ASSERT_EQ(again.size(), earlier.size());
    for (std::size_t i = 0; i < again.size(); ++i) EXPECT_EQ(*again[i], *earlier[i]);
}

}  // namespace
}  // namespace ghostdrive::daemon
