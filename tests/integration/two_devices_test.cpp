// Two complete devices (daemon::Service, without FUSE) in one process on 127.0.0.1: pairing by
// invite code, index exchange, and reading each other's files.

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>
#include <random>

#include "ghostdrive/daemon/drive.hpp"
#include "ghostdrive/daemon/invite.hpp"
#include "ghostdrive/daemon/service.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::daemon {
namespace {

using namespace std::chrono_literals;

core::Bytes random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    core::Bytes data(size);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    return data;
}

ServiceOptions loopback_options() {
    ServiceOptions options;
    options.listen = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0);
    options.invite_addresses = {asio::ip::address_v4::loopback()};
    options.sync_poll = 50ms;
    options.redial = 200ms;
    options.lan_discovery = false;  // the LAN test below turns it on
    return options;
}

class TwoDevices : public ::testing::Test {
protected:
    ~TwoDevices() override {
        if (c_) c_->stop();
        if (b_) b_->stop();
        if (a_) a_->stop();
        drain([] { return false; }, 300ms);
        c_.reset();
        b_.reset();
        a_.reset();
        disk_.join();
    }

    template <typename Done>
    bool drain(Done done, std::chrono::milliseconds limit = 20s) {
        auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < deadline) io_.run_one_for(5ms);
        return done();
    }

    void start_a(ServiceOptions options = loopback_options()) {
        auto drive = Drive::create(dir_.path() / "a", "desk");
        ASSERT_TRUE(drive) << drive.error().message;
        drive_a_ = std::move(drive).value();
        a_ = std::make_unique<Service>(io_.get_executor(), disk_.get_executor(), *drive_a_,
                                       options);
        ASSERT_TRUE(a_->start());
    }

    core::Result<JoinResult> join_with(const std::string& code, const std::string& root) {
        auto invite = decode_invite(code);
        if (!invite) return invite.error();
        auto key = Drive::load_or_create_key(dir_.path() / root);
        if (!key) return key.error();
        transport::EndpointOptions options;
        auto endpoint = transport::Endpoint::create(io_.get_executor(), key.value(), options);
        if (!endpoint) return endpoint.error();
        Join join;
        join.token = invite.value().token;
        join.name = root;
        join.listen_port = 1;  // B dials A in these tests, so A never needs B's port
        std::optional<core::Result<JoinResult>> result;
        asio::co_spawn(
            io_,
            [&]() -> asio::awaitable<void> {
                result = co_await request_join(*endpoint.value(), invite.value(), join);
            },
            asio::detached);
        drain([&] { return result.has_value(); });
        endpoint.value()->close();
        if (!result) return core::Error{core::Errc::timeout, "join timed out"};
        return std::move(*result);
    }

    void start_b() {
        auto code = a_->create_invite(60s);
        ASSERT_TRUE(code);
        auto joined = join_with(code.value(), "b");
        ASSERT_TRUE(joined) << joined.error().message;
        auto drive = Drive::join(dir_.path() / "b", "laptop", joined.value().info,
                                 drive_a_->key().public_key());
        ASSERT_TRUE(drive) << drive.error().message;
        auto addr = joined.value().inviter_address;
        ASSERT_TRUE(drive.value()->set_peer_address(
            drive_a_->key().public_key(),
            addr.address().to_string() + ":" + std::to_string(addr.port())));
        drive_b_ = std::move(drive).value();
        b_ = std::make_unique<Service>(io_.get_executor(), disk_.get_executor(), *drive_b_,
                                       loopback_options());
        ASSERT_TRUE(b_->start());
        ASSERT_TRUE(drain([&] {
            return a_->connected_to(drive_b_->key().public_key()) &&
                   b_->connected_to(drive_a_->key().public_key());
        })) << "devices did not connect";
    }

    // A third device, invited by A. It is given B's address, so the test is about membership
    // reaching B, not about finding B.
    void start_c() {
        auto code = a_->create_invite(60s);
        ASSERT_TRUE(code);
        auto joined = join_with(code.value(), "c");
        ASSERT_TRUE(joined) << joined.error().message;
        auto drive = Drive::join(dir_.path() / "c", "c", joined.value().info,
                                 drive_a_->key().public_key());
        ASSERT_TRUE(drive) << drive.error().message;
        ASSERT_TRUE(drive.value()->set_peer_address(drive_a_->key().public_key(),
                                                    "127.0.0.1:" + std::to_string(a_->port())));
        ASSERT_TRUE(drive.value()->set_peer_address(drive_b_->key().public_key(),
                                                    "127.0.0.1:" + std::to_string(b_->port())));
        drive_c_ = std::move(drive).value();
        c_ = std::make_unique<Service>(io_.get_executor(), disk_.get_executor(), *drive_c_,
                                       loopback_options());
        ASSERT_TRUE(c_->start());
    }

    // Writes a file in the root of `fs`.
    void write_file(vfs::Filesystem& fs, const std::string& name, const core::Bytes& data) {
        auto created = fs.create(vfs::kRootIno, name, 0644);
        ASSERT_TRUE(created) << created.error().message;
        auto h = created.value().second;
        std::optional<core::Status> wrote;
        fs.write(h, 0, data, [&](core::Status st) { wrote = st; });
        ASSERT_TRUE(drain([&] { return wrote.has_value(); }));
        ASSERT_TRUE(*wrote);
        std::optional<core::Status> flushed;
        fs.flush(h, [&](core::Status st) { flushed = st; });
        ASSERT_TRUE(drain([&] { return flushed.has_value(); }));
        ASSERT_TRUE(*flushed) << flushed->error().message;
        fs.release(h);
    }

    std::optional<core::Bytes> read_file(vfs::Filesystem& fs, const std::string& name) {
        auto ino = fs.lookup(vfs::kRootIno, name);
        if (!ino) return std::nullopt;
        auto h = fs.open(*ino);
        if (!h) return std::nullopt;
        auto size = fs.getattr(*ino)->size;
        std::optional<core::Result<vfs::ReadReply>> result;
        fs.read(h.value(), 0, static_cast<std::size_t>(size),
                [&](core::Result<vfs::ReadReply> r) { result = std::move(r); });
        drain([&] { return result.has_value(); });
        fs.release(h.value());
        if (!result || !*result) return std::nullopt;
        core::Bytes out;
        for (auto s : result->value().slices) out.insert(out.end(), s.begin(), s.end());
        return out;
    }

    test::TempDir dir_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_{io_.get_executor()};
    asio::thread_pool disk_{4};
    std::unique_ptr<Drive> drive_a_;
    std::unique_ptr<Drive> drive_b_;
    std::unique_ptr<Drive> drive_c_;
    std::unique_ptr<Service> a_;
    std::unique_ptr<Service> b_;
    std::unique_ptr<Service> c_;
};

TEST_F(TwoDevices, JoinedDeviceSharesTheDriveSecret) {
    start_a();
    start_b();
    EXPECT_EQ(drive_b_->drive_id(), drive_a_->drive_id());
    EXPECT_EQ(drive_b_->secret(), drive_a_->secret());
    EXPECT_TRUE(drive_a_->index().is_pinned(drive_b_->key().public_key()).value());
    EXPECT_TRUE(drive_b_->index().is_pinned(drive_a_->key().public_key()).value());
    auto status = a_->status();
    ASSERT_EQ(status.peers.size(), 1u);
    EXPECT_EQ(status.peers[0].name, "b");
    EXPECT_TRUE(status.peers[0].connected);
}

TEST_F(TwoDevices, FilesWrittenOnOneDeviceAreReadableOnTheOther) {
    start_a();
    start_b();
    auto from_a = random_bytes(700'000, 1);  // several chunks, all only on A
    write_file(a_->filesystem(), "from-desk.bin", from_a);
    // The empty record from create() can arrive before the commit with the data.
    auto synced = [](vfs::Filesystem& fs, const char* name, std::size_t size) {
        auto ino = fs.lookup(vfs::kRootIno, name);
        return ino && fs.getattr(*ino)->size == size;
    };
    ASSERT_TRUE(drain([&] { return synced(b_->filesystem(), "from-desk.bin", from_a.size()); }))
        << "the record never reached B";
    auto got = read_file(b_->filesystem(), "from-desk.bin");
    ASSERT_TRUE(got);
    EXPECT_TRUE(*got == from_a);

    auto from_b = random_bytes(5000, 2);
    write_file(b_->filesystem(), "from-laptop.txt", from_b);
    ASSERT_TRUE(drain([&] { return synced(a_->filesystem(), "from-laptop.txt", from_b.size()); }));
    auto got_a = read_file(a_->filesystem(), "from-laptop.txt");
    ASSERT_TRUE(got_a);
    EXPECT_TRUE(*got_a == from_b);
}

TEST_F(TwoDevices, DeletesPropagate) {
    start_a();
    start_b();
    write_file(a_->filesystem(), "temp.txt", random_bytes(10, 3));
    ASSERT_TRUE(
        drain([&] { return b_->filesystem().lookup(vfs::kRootIno, "temp.txt").has_value(); }));
    ASSERT_TRUE(a_->filesystem().unlink(vfs::kRootIno, "temp.txt"));
    EXPECT_TRUE(
        drain([&] { return !b_->filesystem().lookup(vfs::kRootIno, "temp.txt").has_value(); }));
}

TEST_F(TwoDevices, AnInviteWorksOnlyOnce) {
    start_a();
    auto code = a_->create_invite(60s);
    ASSERT_TRUE(code);
    auto first = join_with(code.value(), "b");
    ASSERT_TRUE(first) << first.error().message;
    auto second = join_with(code.value(), "c");
    // With no invite open, A refuses the unknown key during the handshake itself.
    ASSERT_FALSE(second);
    EXPECT_NE(second.error().code, core::Errc::timeout) << second.error().message;
}

TEST_F(TwoDevices, AForgedTokenIsRejected) {
    start_a();
    auto code = a_->create_invite(60s);
    ASSERT_TRUE(code);
    auto invite = decode_invite(code.value()).value();
    invite.token[0] ^= std::byte{1};
    auto result = join_with(encode_invite(invite), "b");
    ASSERT_FALSE(result);
    EXPECT_FALSE(drive_a_->index().devices().value().size() > 0);
}

TEST_F(TwoDevices, AnImpostorOfTheInviterIsNotTrusted) {
    start_a();
    auto code = a_->create_invite(60s);
    ASSERT_TRUE(code);
    auto invite = decode_invite(code.value()).value();
    invite.inviter[0] ^= std::byte{1};  // a code naming a different key than A's
    auto result = join_with(encode_invite(invite), "b");
    ASSERT_FALSE(result);  // the pinned key does not match, so no secret is ever requested
}

TEST_F(TwoDevices, UnpairedDevicesCannotConnectWithoutAnInvite) {
    start_a();
    auto key = Drive::load_or_create_key(dir_.path() / "stranger").value();
    auto endpoint = transport::Endpoint::create(io_.get_executor(), key, {}).value();
    std::optional<core::Result<std::shared_ptr<transport::Connection>>> conn;
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            conn = co_await endpoint->connect(
                asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), a_->port()),
                drive_a_->key().public_key());
        },
        asio::detached);
    drain([&] { return conn.has_value(); });
    ASSERT_TRUE(conn);
    EXPECT_FALSE(*conn);
    endpoint->close();
}

TEST_F(TwoDevices, DevicesFindEachOtherOnTheLanWithoutStoredAddresses) {
#ifndef GHOSTDRIVE_HAVE_DNSSD
    GTEST_SKIP() << "built without DNS-SD";
#else
    // Bound to every interface, since mDNS hands out the host's LAN address.
    ServiceOptions lan;
    lan.listen = asio::ip::udp::endpoint(asio::ip::address_v4::any(), 0);
    lan.invite_addresses = {asio::ip::address_v4::loopback()};
    lan.sync_poll = 50ms;
    lan.redial = 200ms;
    start_a(lan);

    auto code = a_->create_invite(60s);
    ASSERT_TRUE(code);
    auto joined = join_with(code.value(), "b");
    ASSERT_TRUE(joined) << joined.error().message;
    auto drive = Drive::join(dir_.path() / "b", "laptop", joined.value().info,
                                 drive_a_->key().public_key());
    ASSERT_TRUE(drive);
    // Both sides know only addresses that lead nowhere.
    ASSERT_TRUE(drive.value()->set_peer_address(drive_a_->key().public_key(), "127.0.0.1:9"));
    ASSERT_TRUE(drive_a_->set_peer_address(drive.value()->key().public_key(), "127.0.0.1:9"));
    drive_b_ = std::move(drive).value();
    b_ = std::make_unique<Service>(io_.get_executor(), disk_.get_executor(), *drive_b_, lan);
    ASSERT_TRUE(b_->start());

    ASSERT_TRUE(drain(
        [&] {
            return a_->connected_to(drive_b_->key().public_key()) &&
                   b_->connected_to(drive_a_->key().public_key());
        },
        30s))
        << "mDNS did not bring the devices together";
    // The address found on the LAN replaced the dead one.
    EXPECT_NE(drive_b_->peer_addresses().at(drive_a_->key().public_key()), "127.0.0.1:9");
#endif
}

TEST_F(TwoDevices, ADeviceInvitedByOneMemberIsAcceptedByTheOthers) {
    start_a();
    start_b();
    start_c();
    auto c = drive_c_->key().public_key();
    ASSERT_TRUE(drain([&] {
        return b_->connected_to(c) && c_->connected_to(drive_b_->key().public_key());
    })) << "B never accepted C";
    auto status = b_->status();
    auto it = std::find_if(status.peers.begin(), status.peers.end(),
                           [&](const PeerStatus& p) { return p.key == c; });
    ASSERT_NE(it, status.peers.end());
    EXPECT_EQ(it->name, "c");

    // Files flow between the two devices that were never paired directly.
    auto data = random_bytes(300'000, 9);
    write_file(c_->filesystem(), "from-c.bin", data);
    ASSERT_TRUE(drain([&] {
        auto ino = b_->filesystem().lookup(vfs::kRootIno, "from-c.bin");
        return ino && b_->filesystem().getattr(*ino)->size == data.size();
    }));
    auto got = read_file(b_->filesystem(), "from-c.bin");
    ASSERT_TRUE(got);
    EXPECT_TRUE(*got == data);
}

TEST_F(TwoDevices, ARemovedDeviceIsCutOffEverywhere) {
    start_a();
    start_b();
    start_c();
    auto b = drive_b_->key().public_key();
    auto c = drive_c_->key().public_key();
    ASSERT_TRUE(drain([&] { return b_->connected_to(c) && a_->connected_to(c); }));

    EXPECT_FALSE(a_->remove_member("desk")) << "a device cannot remove itself";
    EXPECT_FALSE(a_->remove_member("nobody"));
    auto removed = a_->remove_member("c");
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_NE(removed.value().find("Removed c"), std::string::npos) << removed.value();

    ASSERT_TRUE(drain([&] {
        return !a_->connected_to(c) && !b_->connected_to(c) &&
               !drive_b_->index().is_pinned(c).value();
    })) << "B still accepts C";
    EXPECT_TRUE(drive_b_->membership().view().removed.contains(c));
    // A tells C why it is being cut off.
    EXPECT_TRUE(drain([&] { return c_->status().removed_by == "desk"; }));

    // C keeps trying its stored addresses; nobody lets it back in.
    drain([] { return false; }, 1500ms);
    EXPECT_FALSE(a_->connected_to(c));
    EXPECT_FALSE(b_->connected_to(c));
    EXPECT_TRUE(a_->connected_to(b));

    // Its key stays out even with a fresh invite.
    auto code = a_->create_invite(60s);
    ASSERT_TRUE(code);
    c_->stop();
    auto rejoin = join_with(code.value(), "c");
    EXPECT_FALSE(rejoin);
}

}  // namespace
}  // namespace ghostdrive::daemon
