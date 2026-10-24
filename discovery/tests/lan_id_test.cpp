#include "ghostdrive/discovery/lan_id.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace ghostdrive::discovery {
namespace {

using namespace std::chrono_literals;

core::DriveSecret secret_with(std::uint8_t fill) {
    core::DriveSecret s{};
    s.fill(std::byte{fill});
    return s;
}

core::PublicKey key_with(std::uint8_t fill) {
    core::PublicKey k{};
    k.fill(std::byte{fill});
    return k;
}

TEST(LanId, IsStableWithinAnEpochAndChangesAcrossEpochs) {
    auto s = secret_with(1);
    auto k = key_with(2);
    EXPECT_EQ(lan_id(s, k, 100), lan_id(s, k, 100));
    EXPECT_NE(lan_id(s, k, 100), lan_id(s, k, 101));
    EXPECT_EQ(lan_id(s, k, 100).size(), 20u);  // 12 bytes of base32
}

TEST(LanId, DependsOnTheDriveSecretAndTheDevice) {
    EXPECT_NE(lan_id(secret_with(1), key_with(2), 7), lan_id(secret_with(3), key_with(2), 7));
    EXPECT_NE(lan_id(secret_with(1), key_with(2), 7), lan_id(secret_with(1), key_with(4), 7));
}

TEST(LanId, MembersAreRecognizedAcrossNeighbouringEpochs) {
    auto s = secret_with(5);
    std::vector<core::PublicKey> members{key_with(1), key_with(2), key_with(3)};
    auto now = std::chrono::system_clock::now();
    auto epoch = lan_epoch(now);
    for (auto e : {epoch - 1, epoch, epoch + 1}) {
        auto found = match_lan_id(lan_id(s, members[1], e), s, members, now);
        ASSERT_TRUE(found);
        EXPECT_EQ(*found, members[1]);
    }
    EXPECT_FALSE(match_lan_id(lan_id(s, members[1], epoch + 2), s, members, now));
}

TEST(LanId, OutsidersAndOtherDrivesAreNotRecognized) {
    auto s = secret_with(5);
    std::vector<core::PublicKey> members{key_with(1)};
    auto now = std::chrono::system_clock::now();
    auto epoch = lan_epoch(now);
    EXPECT_FALSE(match_lan_id(lan_id(secret_with(6), members[0], epoch), s, members, now));
    EXPECT_FALSE(match_lan_id(lan_id(s, key_with(9), epoch), s, members, now));
    EXPECT_FALSE(match_lan_id("", s, members, now));
    EXPECT_FALSE(match_lan_id("garbage", s, members, now));
}

TEST(LanId, EpochIsAnHour) {
    std::chrono::system_clock::time_point t{std::chrono::seconds(3600 * 10 + 5)};
    EXPECT_EQ(lan_epoch(t), 10u);
    EXPECT_EQ(lan_epoch(t + 3595s), 11u);
}

}  // namespace
}  // namespace ghostdrive::discovery
