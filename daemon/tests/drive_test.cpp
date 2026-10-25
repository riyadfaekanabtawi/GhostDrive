#include "ghostdrive/daemon/drive.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "temp_dir.hpp"

namespace ghostdrive::daemon {
namespace {

std::uint32_t mode_of(const std::filesystem::path& p) {
    struct stat st{};
    EXPECT_EQ(::stat(p.c_str(), &st), 0) << p;
    return st.st_mode & 0777;
}

TEST(Drive, CreateThenOpenGivesTheSameDrive) {
    test::TempDir dir;
    auto root = dir.path() / "a";
    auto created = Drive::create(root, "desk");
    ASSERT_TRUE(created) << created.error().message;
    auto id = created.value()->drive_id();
    auto secret = created.value()->secret();
    auto key = created.value()->key().public_key();
    created.value().reset();

    auto opened = Drive::open(root);
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(opened.value()->drive_id(), id);
    EXPECT_EQ(opened.value()->secret(), secret);
    EXPECT_EQ(opened.value()->key().public_key(), key);
    EXPECT_EQ(opened.value()->device_name(), "desk");
    EXPECT_EQ(opened.value()->index().local_device(), core::short_id(key));
}

TEST(Drive, SecretsArePrivate) {
    test::TempDir dir;
    auto root = dir.path() / "a";
    ASSERT_TRUE(Drive::create(root, "desk"));
    DrivePaths paths(root);
    EXPECT_EQ(mode_of(paths.drive), 0600u);
    EXPECT_EQ(mode_of(paths.key), 0600u);
    EXPECT_EQ(mode_of(paths.discovery), 0600u);
    EXPECT_EQ(mode_of(paths.root), 0700u);

    ASSERT_EQ(::chmod(paths.drive.c_str(), 0644), 0);
    auto opened = Drive::open(root);
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, core::Errc::permission);
}

TEST(Drive, CreateRefusesToReplaceAnExistingDrive) {
    test::TempDir dir;
    ASSERT_TRUE(Drive::create(dir.path() / "a", "desk"));
    auto again = Drive::create(dir.path() / "a", "desk");
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, core::Errc::already_exists);
    EXPECT_FALSE(Drive::open(dir.path() / "missing"));
}

TEST(Drive, JoinPinsTheMembersTheInviterVouchesFor) {
    test::TempDir dir;
    auto inviter = Drive::create(dir.path() / "a", "desk");
    ASSERT_TRUE(inviter);
    auto& a = *inviter.value();
    auto phone = core::DeviceKey::generate();
    ASSERT_TRUE(phone);
    ASSERT_TRUE(a.membership().add(a.key(), phone.value().public_key(), "phone", true));
    core::PublicKey stranger{};
    stranger.fill(std::byte{9});

    DriveInfo info;
    info.drive_id = a.drive_id();
    info.secret = a.secret();
    info.members.push_back(Member{a.key().public_key(), "desk", ""});
    info.members.push_back(Member{phone.value().public_key(), "phone", "10.0.0.9:7373"});
    // Listed, but no member vouches for it.
    info.members.push_back(Member{stranger, "stranger", "10.0.0.10:7373"});
    info.membership = a.membership().entries();
    // The inviter replaced its discovery key after removing a device.
    core::PublicKey gone{};
    gone.fill(std::byte{8});
    ASSERT_TRUE(a.discovery_keys().update(a.key(), {phone.value().public_key()}, {gone}));
    info.discovery_keys = a.discovery_keys().keys();

    auto joined = Drive::join(dir.path() / "b", "laptop", info, a.key().public_key());
    ASSERT_TRUE(joined) << joined.error().message;
    auto& b = *joined.value();
    EXPECT_EQ(b.drive_id(), info.drive_id);
    EXPECT_TRUE(b.index().is_pinned(a.key().public_key()).value());
    EXPECT_TRUE(b.index().is_pinned(phone.value().public_key()).value());
    EXPECT_FALSE(b.index().is_pinned(stranger).value());
    EXPECT_EQ(b.peer_addresses().at(phone.value().public_key()), "10.0.0.9:7373");
    EXPECT_FALSE(b.peer_addresses().contains(stranger));
    EXPECT_EQ(b.discovery_keys().current(), a.discovery_keys().current());

    // The membership and the discovery keys are stored and survive reopening.
    auto key_b = b.key().public_key();
    joined.value().reset();
    auto reopened = Drive::open(dir.path() / "b");
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto view = reopened.value()->membership().view();
    EXPECT_TRUE(view.members.contains(a.key().public_key()));
    EXPECT_TRUE(view.members.contains(phone.value().public_key()));
    EXPECT_EQ(view.members.size(), 2u);
    EXPECT_EQ(reopened.value()->key().public_key(), key_b);
    EXPECT_EQ(reopened.value()->discovery_keys().current(), a.discovery_keys().current());
}

TEST(Drive, OpeningAnOlderDriveVouchesForItsPinnedDevices) {
    test::TempDir dir;
    auto root = dir.path() / "a";
    core::PublicKey other{};
    other.fill(std::byte{5});
    {
        auto created = Drive::create(root, "desk");
        ASSERT_TRUE(created);
        ASSERT_TRUE(created.value()->index().add_device(other, "phone"));
    }
    std::filesystem::remove(DrivePaths(root).membership);
    auto opened = Drive::open(root);
    ASSERT_TRUE(opened) << opened.error().message;
    auto view = opened.value()->membership().view();
    ASSERT_TRUE(view.members.contains(other));
    EXPECT_EQ(view.members.at(other), "phone");
    EXPECT_TRUE(std::filesystem::exists(DrivePaths(root).membership));
}

TEST(Drive, PeerAddressesPersist) {
    test::TempDir dir;
    auto root = dir.path() / "a";
    auto drive = Drive::create(root, "desk");
    ASSERT_TRUE(drive);
    core::PublicKey k{};
    k.fill(std::byte{4});
    ASSERT_TRUE(drive.value()->set_peer_address(k, "192.168.0.5:7373"));
    ASSERT_TRUE(drive.value()->set_peer_address(k, "192.168.0.6:7373"));
    drive.value().reset();
    auto opened = Drive::open(root);
    ASSERT_TRUE(opened);
    auto peers = opened.value()->peer_addresses();
    ASSERT_EQ(peers.size(), 1u);
    EXPECT_EQ(peers.at(k), "192.168.0.6:7373");
}

TEST(Drive, LongDataDirectoriesStillGetAUsableSocketPath) {
    std::filesystem::path deep = "/" + std::string(150, 'x');
    DrivePaths paths(deep);
    // Either a short path in a per-user private directory, or (with none available) the
    // preferred path, which then fails to bind with a clear error instead of using /tmp.
    EXPECT_TRUE(paths.control.string().size() < 104u || paths.control == deep / "control.sock");
    EXPECT_EQ(paths.control.string().find("/tmp/"), std::string::npos);
}

TEST(Drive, PeerAddressesMustLookLikeAddresses) {
    test::TempDir dir;
    auto drive = Drive::create(dir.path() / "a", "desk");
    ASSERT_TRUE(drive);
    core::PublicKey k{};
    EXPECT_FALSE(drive.value()->set_peer_address(k, "1.2.3.4:5 extra"));
    EXPECT_FALSE(drive.value()->set_peer_address(k, "1.2.3.4:5\nffff 6.6.6.6:1"));
    EXPECT_FALSE(drive.value()->set_peer_address(k, ""));
    EXPECT_TRUE(drive.value()->set_peer_address(k, "[fe80::1]:7373"));
}

TEST(Drive, OpenRefusesADataDirectoryOthersCanRead) {
    test::TempDir dir;
    auto root = dir.path() / "a";
    ASSERT_TRUE(Drive::create(root, "desk"));
    ASSERT_EQ(::chmod(root.c_str(), 0755), 0);
    auto opened = Drive::open(root);
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, core::Errc::permission);
}

}  // namespace
}  // namespace ghostdrive::daemon
