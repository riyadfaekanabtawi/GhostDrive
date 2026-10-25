// Two complete devices (daemon::Service, without FUSE) in one process on 127.0.0.1: pairing by
// invite code, index exchange, and reading each other's files.

#include <gtest/gtest.h>

#include <algorithm>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>
#include <random>

#include "ghostdrive/daemon/drive.hpp"
#include "ghostdrive/daemon/invite.hpp"
#include "ghostdrive/daemon/service.hpp"
#include "ghostdrive/discovery/lan_discovery.hpp"
#include "ghostdrive/transport/frame.hpp"
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
        if (d_) d_->stop();
        if (c_) c_->stop();
        if (b_) b_->stop();
        if (a_) a_->stop();
        drain([] { return false; }, 300ms);
        d_.reset();
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
                                       options());
        ASSERT_TRUE(b_->start());
        ASSERT_TRUE(drain([&] {
            return a_->connected_to(drive_b_->key().public_key()) &&
                   b_->connected_to(drive_a_->key().public_key());
        })) << "devices did not connect";
    }

    // Another device, invited by A. It is given B's address, so the tests are about
    // membership reaching B, not about finding B.
    void join_by_invite(const std::string& root, std::unique_ptr<Drive>& out) {
        auto code = a_->create_invite(60s);
        ASSERT_TRUE(code);
        auto joined = join_with(code.value(), root);
        ASSERT_TRUE(joined) << joined.error().message;
        auto drive = Drive::join(dir_.path() / root, root, joined.value().info,
                                 drive_a_->key().public_key());
        ASSERT_TRUE(drive) << drive.error().message;
        ASSERT_TRUE(drive.value()->set_peer_address(drive_a_->key().public_key(),
                                                    "127.0.0.1:" + std::to_string(a_->port())));
        ASSERT_TRUE(drive.value()->set_peer_address(drive_b_->key().public_key(),
                                                    "127.0.0.1:" + std::to_string(b_->port())));
        out = std::move(drive).value();
    }

    std::unique_ptr<Service> start_service(Drive& drive) {
        auto service = std::make_unique<Service>(io_.get_executor(), disk_.get_executor(), drive,
                                                 options());
        EXPECT_TRUE(service->start());
        return service;
    }

    void start_c() {
        join_by_invite("c", drive_c_);
        ASSERT_TRUE(drive_c_);
        c_ = start_service(*drive_c_);
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
    std::unique_ptr<Drive> drive_d_;
    std::unique_ptr<Service> a_;
    std::unique_ptr<Service> b_;
    std::unique_ptr<Service> c_;
    std::unique_ptr<Service> d_;
    std::chrono::milliseconds countersign_after_ = loopback_options().countersign_after;
    std::chrono::milliseconds pending_hold_ = loopback_options().pending_hold;

    ServiceOptions options() const {
        auto o = loopback_options();
        o.countersign_after = countersign_after_;
        o.pending_hold = pending_hold_;
        return o;
    }
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
#ifndef GHOSTDRIVE_HAVE_LAN_DISCOVERY
    GTEST_SKIP() << "built without LAN discovery";
#else
    // Built with it, but the responder may be missing (avahi-daemon on CI runners).
    if (auto probe = discovery::LanDiscovery::start(io_.get_executor(), 9,
                                                    [] { return std::string("probe"); }, {});
        !probe) {
        GTEST_SKIP() << "no mDNS responder: " << probe.error().message;
    }
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

    // A and B move to a discovery key C never gets, so it can no longer find them on the LAN
    // or meet them at a rendezvous server.
    ASSERT_TRUE(drain([&] {
        return drive_a_->discovery_keys().current() == drive_b_->discovery_keys().current();
    }));
    auto current = drive_a_->discovery_keys().current();
    EXPECT_NE(current, drive_a_->secret());
    for (const auto& k : drive_c_->discovery_keys().all()) EXPECT_NE(*k.key, current);

    // Its key stays out even with a fresh invite.
    auto code = a_->create_invite(60s);
    ASSERT_TRUE(code);
    c_->stop();
    auto rejoin = join_with(code.value(), "c");
    EXPECT_FALSE(rejoin);
}

TEST_F(TwoDevices, ForgedRecordsFromAMemberAreDropped) {
    start_a();
    start_b();
    auto data = random_bytes(5000, 11);
    write_file(a_->filesystem(), "a.txt", data);
    ASSERT_TRUE(drain([&] {
        auto r = drive_b_->index().get("a.txt");
        return r && r.value() && r.value()->size == data.size();
    }));
    const auto genuine = drive_a_->index().get("a.txt").value().value();

    // C is a member but misbehaves: instead of running a daemon it sends B made-up records.
    join_by_invite("c", drive_c_);
    ASSERT_TRUE(drive_c_);
    const auto c = drive_c_->key().public_key();
    ASSERT_TRUE(drain([&] { return drive_b_->index().is_pinned(c).value(); }));
    const auto a_id = core::short_id(drive_a_->key().public_key());
    const auto& drive = drive_a_->drive_id();
    auto sign_as_c = [&](core::FileRecord r) {
        r.signature = drive_c_->key().sign(core::record_signed_bytes(drive, r)).value();
        return r;
    };
    auto altered = genuine;  // A's record with a new version, under A's old signature
    altered.version.bump(a_id);
    altered.mtime_ns += 1;
    core::FileRecord made_up;  // a file A never wrote, signed by C in A's name
    made_up.path = "made-up.txt";
    made_up.version.bump(a_id);
    made_up.modified_by = a_id;
    made_up.hlc = drive_c_->clock().now();
    made_up = sign_as_c(made_up);
    core::FileRecord own;  // and one honest record, sent last
    own.path = "from-c.txt";
    own.version.bump(core::short_id(c));
    own.modified_by = core::short_id(c);
    own.hlc = drive_c_->clock().now();
    own = sign_as_c(own);
    altered.sequence = 1;
    made_up.sequence = 2;
    own.sequence = 3;
    std::vector<IndexEntry> batch{{altered, false}, {made_up, false}, {own, false}};

    auto endpoint = transport::Endpoint::create(io_.get_executor(), drive_c_->key(), {}).value();
    std::optional<bool> sent;
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            auto conn = co_await endpoint->connect(
                asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), b_->port()),
                drive_b_->key().public_key());
            if (!conn) {
                sent = false;
                co_return;
            }
            auto stream = co_await conn.value()->open_stream();
            if (!stream) {
                sent = false;
                co_return;
            }
            auto hello = encode_hello(Hello{7, 3});
            if (!co_await stream.value()->write(hello)) {
                sent = false;
                co_return;
            }
            auto request = co_await transport::wire::read_frame(*stream.value(), 64);
            if (!request) {
                sent = false;
                co_return;
            }
            auto frame = encode_index_batch(batch);
            sent = static_cast<bool>(co_await stream.value()->write(frame));
            // Stay connected until the test is done with B.
            co_await asio::steady_timer(io_, 10s).async_wait(asio::use_awaitable);
        },
        asio::detached);
    ASSERT_TRUE(drain([&] { return sent.has_value(); }));
    ASSERT_TRUE(*sent);
    ASSERT_TRUE(drain([&] {
        return b_->filesystem().lookup(vfs::kRootIno, "from-c.txt").has_value();
    })) << "B never applied C's own record";
    EXPECT_FALSE(drive_b_->index().get("made-up.txt").value().has_value());
    EXPECT_FALSE(b_->filesystem().lookup(vfs::kRootIno, "made-up.txt").has_value());
    auto at_b = drive_b_->index().get("a.txt").value().value();
    EXPECT_EQ(at_b.version, genuine.version);
    EXPECT_EQ(at_b.mtime_ns, genuine.mtime_ns);
    endpoint->close();
}

TEST_F(TwoDevices, RecordsByADeviceNotKnownYetAreFetchedAgain) {
    start_a();
    start_b();
    join_by_invite("c", drive_c_);
    ASSERT_TRUE(drive_c_);
    const auto c = drive_c_->key().public_key();
    ASSERT_TRUE(drain([&] { return drive_b_->index().is_pinned(c).value(); }));
    // E is not a member yet: the membership entries adding it can lag behind its records.
    auto e_key = Drive::load_or_create_key(dir_.path() / "e").value();
    const auto& drive = drive_a_->drive_id();
    auto make = [&](const core::DeviceKey& key, const char* path, std::uint64_t sequence) {
        core::FileRecord r;
        r.path = path;
        r.modified_by = core::short_id(key.public_key());
        r.version.bump(r.modified_by);
        r.hlc = drive_c_->clock().now();
        r.sequence = sequence;
        r.signature = key.sign(core::record_signed_bytes(drive, r)).value();
        return r;
    };
    std::vector<IndexEntry> batch{{make(e_key, "from-e.txt", 1), false},
                                  {make(drive_c_->key(), "from-c.txt", 2), false}};

    auto endpoint = transport::Endpoint::create(io_.get_executor(), drive_c_->key(), {}).value();
    std::optional<bool> sent;
    std::shared_ptr<transport::Connection> held;
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            auto conn = co_await endpoint->connect(
                asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), b_->port()),
                drive_b_->key().public_key());
            if (!conn) {
                sent = false;
                co_return;
            }
            held = conn.value();
            auto stream = co_await conn.value()->open_stream();
            if (!stream || !co_await stream.value()->write(encode_hello(Hello{7, 2})) ||
                !co_await transport::wire::read_frame(*stream.value(), 64)) {
                sent = false;
                co_return;
            }
            sent = static_cast<bool>(co_await stream.value()->write(encode_index_batch(batch)));
        },
        asio::detached);
    ASSERT_TRUE(drain([&] { return sent.has_value(); }));
    ASSERT_TRUE(*sent);
    ASSERT_TRUE(
        drain([&] { return b_->filesystem().lookup(vfs::kRootIno, "from-c.txt").has_value(); }));
    EXPECT_FALSE(drive_b_->index().get("from-e.txt").value().has_value());
    // B's progress stops before E's record, so the next exchange sends it again.
    auto devices = drive_b_->index().devices();
    ASSERT_TRUE(devices);
    for (const auto& d : devices.value()) {
        if (d.key == c) EXPECT_EQ(d.peer_max_sequence, 0u);
    }

    // When E is pinned, B drops the connection so the records come again.
    join_by_invite("e", drive_d_);
    ASSERT_TRUE(drain([&] { return drive_b_->index().is_pinned(e_key.public_key()).value(); }));
    EXPECT_TRUE(drain([&] { return !held->is_open(); }));
    endpoint->close();
}

TEST_F(TwoDevices, RecordsSkippedAfterTheHoldAreFetchedWhenTheDeviceArrives) {
    pending_hold_ = 300ms;
    start_a();
    start_b();
    join_by_invite("c", drive_c_);
    ASSERT_TRUE(drive_c_);
    const auto c = drive_c_->key().public_key();
    ASSERT_TRUE(drain([&] { return drive_b_->index().is_pinned(c).value(); }));
    auto e_key = Drive::load_or_create_key(dir_.path() / "e").value();
    const auto& drive = drive_a_->drive_id();
    auto make = [&](const core::DeviceKey& key, const char* path, std::uint64_t sequence) {
        core::FileRecord r;
        r.path = path;
        r.modified_by = core::short_id(key.public_key());
        r.version.bump(r.modified_by);
        r.hlc = drive_c_->clock().now();
        r.sequence = sequence;
        r.signature = key.sign(core::record_signed_bytes(drive, r)).value();
        return r;
    };
    std::vector<IndexEntry> first{{make(e_key, "from-e.txt", 1), false}};
    std::vector<IndexEntry> second{{make(drive_c_->key(), "from-c.txt", 2), false}};
    auto progress = [&] {
        auto devices = drive_b_->index().devices();
        for (const auto& d : devices.value()) {
            if (d.key == c) return d.peer_max_sequence;
        }
        return std::uint64_t{99};
    };

    auto endpoint = transport::Endpoint::create(io_.get_executor(), drive_c_->key(), {}).value();
    std::shared_ptr<transport::Stream> stream;
    std::shared_ptr<transport::Connection> held;
    bool ready = false;
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            auto conn = co_await endpoint->connect(
                asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), b_->port()),
                drive_b_->key().public_key());
            if (!conn) co_return;
            held = conn.value();
            auto opened = co_await conn.value()->open_stream();
            if (!opened || !co_await opened.value()->write(encode_hello(Hello{7, 2})) ||
                !co_await transport::wire::read_frame(*opened.value(), 64) ||
                !co_await opened.value()->write(encode_index_batch(first))) {
                co_return;
            }
            stream = opened.value();
            ready = true;
        },
        asio::detached);
    ASSERT_TRUE(drain([&] { return ready; }));
    drain([] { return false; }, 500ms);  // E's record is held, then the hold runs out
    EXPECT_EQ(progress(), 0u);
    bool sent = false;
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            sent = static_cast<bool>(co_await stream->write(encode_index_batch(second)));
        },
        asio::detached);
    ASSERT_TRUE(drain([&] {
        return sent && b_->filesystem().lookup(vfs::kRootIno, "from-c.txt").has_value();
    }));
    // The stream moved past E's record, remembering it was skipped.
    ASSERT_TRUE(drain([&] { return progress() == 2; }));

    // When E is pinned, B fetches C's index again from the start.
    join_by_invite("e", drive_d_);
    ASSERT_TRUE(drain([&] { return drive_b_->index().is_pinned(e_key.public_key()).value(); }));
    EXPECT_TRUE(drain([&] { return progress() == 0 && !held->is_open(); }));
    endpoint->close();
}

TEST_F(TwoDevices, ANewMemberReadsTheFilesOfARemovedDevice) {
    // A removal counts for good, and records are adopted, once it has stood this long.
    countersign_after_ = 1s;
    start_a(options());
    start_b();
    start_c();
    auto c = drive_c_->key().public_key();
    ASSERT_TRUE(drain([&] { return b_->connected_to(c) && a_->connected_to(c); }));
    // A holds the same content, so the chunks outlive C.
    auto data = random_bytes(300'000, 12);
    write_file(a_->filesystem(), "on-a.bin", data);
    write_file(c_->filesystem(), "from-c.bin", data);
    auto has_file = [&](Drive& drive, const char* path) {
        auto r = drive.index().get(path);
        return r && r.value() && r.value()->size == data.size();
    };
    ASSERT_TRUE(drain(
        [&] { return has_file(*drive_a_, "from-c.bin") && has_file(*drive_b_, "from-c.bin"); }));

    ASSERT_TRUE(a_->remove_member("c"));
    ASSERT_TRUE(drain([&] { return !drive_b_->index().is_pinned(c).value(); }));
    c_->stop();
    // Both remaining members adopt C's record once its removal stands.
    const auto c_id = core::short_id(c);
    ASSERT_TRUE(drain([&] {
        auto at_a = drive_a_->index().get("from-c.bin").value();
        auto at_b = drive_b_->index().get("from-c.bin").value();
        return at_a && at_b && at_a->countersigner != 0 && at_b->countersigner != 0;
    }));

    // D joins after the removal and never learns C's key.
    join_by_invite("d", drive_d_);
    ASSERT_TRUE(drive_d_);
    EXPECT_FALSE(drive_d_->index().is_active_signer(c_id));
    d_ = start_service(*drive_d_);
    ASSERT_TRUE(drain([&] {
        auto ino = d_->filesystem().lookup(vfs::kRootIno, "from-c.bin");
        return ino && d_->filesystem().getattr(*ino)->size == data.size();
    })) << "D never got the removed device's file";
    // Only A holds the chunks. B's adopted copy can reach D first, and B does not hold them,
    // so D can read once A's index has told it so.
    ASSERT_TRUE(drain([&] {
        auto r = drive_d_->index().get("from-c.bin");
        if (!r || !r.value()) return false;
        return std::all_of(r.value()->chunks.begin(), r.value()->chunks.end(), [&](auto& id) {
            auto h = drive_d_->index().holders(id);
            return h && !h.value().empty();
        });
    })) << "D never learned that A holds the chunks";
    auto got = read_file(d_->filesystem(), "from-c.bin");
    ASSERT_TRUE(got);
    EXPECT_TRUE(*got == data);
    // Two adoptions of the same content are not a conflict.
    drain([] { return false; }, 500ms);
    auto entries = d_->filesystem().readdir(vfs::kRootIno);
    ASSERT_TRUE(entries);
    for (const auto& e : *entries) {
        EXPECT_EQ(e.name.find(".sync-conflict-"), std::string::npos) << e.name;
    }
}

}  // namespace
}  // namespace ghostdrive::daemon
