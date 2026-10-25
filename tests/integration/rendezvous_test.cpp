// Devices that know no address for each other, brought together by an in-process rendezvous
// server on 127.0.0.1: hole punching (direct on loopback), and the relay.

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
#include "ghostdrive/discovery/rendezvous_codec.hpp"
#include "ghostdrive/rendezvous/server.hpp"
#include "ghostdrive/transport/demux.hpp"
#include "ghostdrive/transport/frame.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::daemon {
namespace {

using namespace std::chrono_literals;
namespace wire = transport::wire;

core::Bytes random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    core::Bytes data(size);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    return data;
}

class Rendezvous : public ::testing::Test {
protected:
    Rendezvous() {
        server_key_ = std::make_unique<core::DeviceKey>(core::DeviceKey::generate().value());
        rendezvous::ServerOptions options;
        options.listen = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0);
        server_ = rendezvous::Server::start(io_.get_executor(), *server_key_, options).value();
    }

    ~Rendezvous() override {
        for (auto* s : {b_.get(), a_.get()}) {
            if (s) s->stop();
        }
        server_->stop();
        drain([] { return false; }, 300ms);
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

    ServiceOptions options(bool relay_only) {
        ServiceOptions o;
        o.listen = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0);
        o.invite_addresses = {asio::ip::address_v4::loopback()};
        o.sync_poll = 50ms;
        o.redial = 200ms;
        o.lan_discovery = false;  // only the rendezvous may bring them together
        o.rendezvous_interval = 300ms;
        o.relay_after = 500ms;
        o.relay_only = relay_only;
        return o;
    }

    RendezvousConfig server_config() const {
        return RendezvousConfig{"127.0.0.1", server_->port(), server_key_->public_key()};
    }

    // A creates the drive and points it at the server; B joins by invite, which carries the
    // rendezvous setting along. Afterwards neither knows a working address for the other.
    void start_pair(bool relay_only) {
        auto a = Drive::create(dir_.path() / "a", "desk");
        ASSERT_TRUE(a);
        drive_a_ = std::move(a).value();
        ASSERT_TRUE(drive_a_->set_rendezvous(server_config()));
        a_ = std::make_unique<Service>(io_.get_executor(), disk_.get_executor(), *drive_a_,
                                       options(relay_only));
        ASSERT_TRUE(a_->start());

        auto code = a_->create_invite(60s);
        ASSERT_TRUE(code);
        auto invite = decode_invite(code.value()).value();
        auto key_b = Drive::load_or_create_key(dir_.path() / "b").value();
        auto endpoint = transport::Endpoint::create(io_.get_executor(), key_b, {}).value();
        std::optional<core::Result<JoinResult>> joined;
        Join join;
        join.token = invite.token;
        join.name = "laptop";
        join.listen_port = 1;
        asio::co_spawn(
            io_,
            [&]() -> asio::awaitable<void> {
                joined = co_await request_join(*endpoint, invite, join);
            },
            asio::detached);
        ASSERT_TRUE(drain([&] { return joined.has_value(); }));
        endpoint->close();
        ASSERT_TRUE(*joined) << joined->error().message;
        auto b = Drive::join(dir_.path() / "b", "laptop", joined->value().info,
                             drive_a_->key().public_key());
        ASSERT_TRUE(b);
        drive_b_ = std::move(b).value();
        ASSERT_TRUE(drive_b_->rendezvous()) << "the invite should carry the rendezvous setting";
        ASSERT_TRUE(drive_b_->set_peer_address(drive_a_->key().public_key(), "127.0.0.1:9"));
        ASSERT_TRUE(drive_a_->set_peer_address(drive_b_->key().public_key(), "127.0.0.1:9"));

        b_ = std::make_unique<Service>(io_.get_executor(), disk_.get_executor(), *drive_b_,
                                       options(relay_only));
        ASSERT_TRUE(b_->start());
        ASSERT_TRUE(drain([&] {
            return a_->connected_to(drive_b_->key().public_key()) &&
                   b_->connected_to(drive_a_->key().public_key());
        })) << "the rendezvous did not bring the devices together";
    }

    bool relayed(Service& s, const core::PublicKey& peer) {
        for (const auto& p : s.status().peers) {
            if (p.key == peer) return p.relayed;
        }
        return false;
    }

    void write_file(vfs::Filesystem& fs, const std::string& name, const core::Bytes& data) {
        auto created = fs.create(vfs::kRootIno, name, 0644);
        ASSERT_TRUE(created);
        auto h = created.value().second;
        std::optional<core::Status> st;
        fs.write(h, 0, data, [&](core::Status s) { st = s; });
        ASSERT_TRUE(drain([&] { return st.has_value(); }) && *st);
        st.reset();
        fs.flush(h, [&](core::Status s) { st = s; });
        ASSERT_TRUE(drain([&] { return st.has_value(); }) && *st);
        fs.release(h);
    }

    std::optional<core::Bytes> read_when_synced(vfs::Filesystem& fs, const std::string& name,
                                                std::size_t size) {
        if (!drain([&] {
                auto ino = fs.lookup(vfs::kRootIno, name);
                return ino && fs.getattr(*ino)->size == size;
            })) {
            return std::nullopt;
        }
        auto ino = fs.lookup(vfs::kRootIno, name);
        auto h = fs.open(*ino);
        std::optional<core::Result<vfs::ReadReply>> r;
        fs.read(h.value(), 0, size, [&](core::Result<vfs::ReadReply> x) { r = std::move(x); });
        drain([&] { return r.has_value(); });
        fs.release(h.value());
        if (!r || !*r) return std::nullopt;
        core::Bytes out;
        for (auto s : r->value().slices) out.insert(out.end(), s.begin(), s.end());
        return out;
    }

    // Registers a fresh key in `group` and asks where `peer` is.
    std::optional<discovery::PeerAt> look_up(const core::DriveSecret& group_key,
                                             const core::PublicKey& peer) {
        auto key = core::DeviceKey::generate().value();
        auto endpoint = transport::Endpoint::create(io_.get_executor(), key, {}).value();
        std::optional<discovery::PeerAt> answer;
        bool done = false;
        asio::co_spawn(
            io_,
            [&]() -> asio::awaitable<void> {
                auto conn = co_await endpoint->connect(
                    asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), server_->port()),
                    server_key_->public_key());
                if (conn) {
                    auto stream = co_await conn.value()->open_stream();
                    auto group = discovery::rendezvous_group(group_key).value();
                    auto admission = discovery::rendezvous_group(drive_a_->secret()).value();
                    if (stream &&
                        co_await stream.value()->write(
                            discovery::encode(discovery::Register{group, admission, {}})) &&
                        co_await stream.value()->write(
                            discovery::encode(discovery::Connect{peer}))) {
                        for (int i = 0; i < 2; ++i) {
                            auto reply = co_await wire::read_frame(*stream.value(), 64);
                            if (reply &&
                                reply.value().type == wire::FrameType::rendezvous_peer_at) {
                                answer = discovery::decode_peer_at(reply.value().payload).value();
                                break;
                            }
                        }
                    }
                    conn.value()->close();
                }
                done = true;
            },
            asio::detached);
        drain([&] { return done; });
        endpoint->close();
        return answer;
    }

    // A replaces its discovery key, as it does after removing a device.
    void rotate_a() {
        core::PublicKey removed{};
        removed.fill(std::byte{0x42});
        auto changed = drive_a_->discovery_keys().update(drive_a_->key(),
                                                         {drive_b_->key().public_key()}, {removed});
        ASSERT_TRUE(changed && changed.value());
    }

    test::TempDir dir_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_{io_.get_executor()};
    asio::thread_pool disk_{4};
    std::unique_ptr<core::DeviceKey> server_key_;
    std::unique_ptr<rendezvous::Server> server_;
    std::unique_ptr<Drive> drive_a_;
    std::unique_ptr<Drive> drive_b_;
    std::unique_ptr<Service> a_;
    std::unique_ptr<Service> b_;
};

TEST_F(Rendezvous, HolePunchingConnectsDevicesWithNoKnownAddress) {
    start_pair(false);
    EXPECT_FALSE(relayed(*a_, drive_b_->key().public_key()));
    EXPECT_FALSE(relayed(*b_, drive_a_->key().public_key()));
    EXPECT_TRUE(a_->status().observed.has_value());

    auto data = random_bytes(300'000, 1);
    write_file(a_->filesystem(), "direct.bin", data);
    auto got = read_when_synced(b_->filesystem(), "direct.bin", data.size());
    ASSERT_TRUE(got);
    EXPECT_TRUE(*got == data);
}

TEST_F(Rendezvous, TheRelayCarriesEverythingWhenThereIsNoDirectPath) {
    start_pair(true);
    EXPECT_TRUE(relayed(*a_, drive_b_->key().public_key()));
    EXPECT_TRUE(relayed(*b_, drive_a_->key().public_key()));

    auto data = random_bytes(500'000, 2);
    write_file(a_->filesystem(), "relayed.bin", data);
    auto got = read_when_synced(b_->filesystem(), "relayed.bin", data.size());
    ASSERT_TRUE(got);
    EXPECT_TRUE(*got == data);
    EXPECT_GT(server_->stats().relayed_bytes, data.size());  // the chunks went through it
}

TEST_F(Rendezvous, ANewDiscoveryKeySpreadsWithoutDroppingTheRelay) {
    start_pair(true);
    auto b = drive_b_->key().public_key();
    auto old_key = drive_a_->discovery_keys().current();
    rotate_a();
    bool dropped = false;
    ASSERT_TRUE(drain([&] {
        dropped = dropped || !a_->connected_to(b);
        return drive_b_->discovery_keys().current() == drive_a_->discovery_keys().current();
    })) << "B never got A's new key";
    EXPECT_FALSE(dropped) << "re-registering tore down the relayed connection";
    EXPECT_TRUE(relayed(*a_, b));

    // Both moved to the new group; the old one no longer finds them.
    drain([] { return false; }, 500ms);
    auto in_old = look_up(old_key, drive_a_->key().public_key());
    ASSERT_TRUE(in_old);
    EXPECT_FALSE(in_old->address);
    auto in_new = look_up(drive_a_->discovery_keys().current(), drive_a_->key().public_key());
    ASSERT_TRUE(in_new);
    EXPECT_TRUE(in_new->address);
}

TEST_F(Rendezvous, AMemberThatMissedANewKeyIsStillFound) {
    start_pair(true);
    auto b = drive_b_->key().public_key();
    auto old_key = drive_a_->discovery_keys().current();
    // B goes offline; meanwhile A replaces its key.
    b_->stop();
    drain([] { return false; }, 300ms);
    b_.reset();
    rotate_a();
    // A moves to the new group (within a poll interval) and only looks into the old one.
    ASSERT_TRUE(drain([&] {
        auto found = look_up(old_key, drive_a_->key().public_key());
        return found && !found->address;
    }));

    // B comes back with the old key, in the old group, and A looks for it there.
    ASSERT_EQ(drive_b_->discovery_keys().current(), old_key);
    b_ = std::make_unique<Service>(io_.get_executor(), disk_.get_executor(), *drive_b_,
                                   options(true));
    ASSERT_TRUE(b_->start());
    ASSERT_TRUE(drain([&] {
        return a_->connected_to(b) && b_->connected_to(drive_a_->key().public_key());
    })) << "A never found B in the group of its earlier key";
    EXPECT_TRUE(drain([&] {
        return drive_b_->discovery_keys().current() == drive_a_->discovery_keys().current();
    }));

    // Looking in an earlier group does not make A findable there: a removed device that knows
    // the old key learns nothing.
    auto found = look_up(old_key, drive_a_->key().public_key());
    ASSERT_TRUE(found);
    EXPECT_FALSE(found->address) << "A was visible in the old group";
}

// A raw client of the server, to probe what it allows.
struct Probe {
    std::shared_ptr<transport::Endpoint> endpoint;
    std::shared_ptr<transport::Connection> conn;
    std::shared_ptr<transport::Stream> control;
};

TEST_F(Rendezvous, OtherGroupsLearnNothingAndCannotRelay) {
    start_pair(true);
    auto stranger_key = core::DeviceKey::generate().value();
    Probe probe;
    probe.endpoint = transport::Endpoint::create(io_.get_executor(), stranger_key, {}).value();
    std::optional<discovery::PeerAt> answer;
    bool registered = false;
    core::DriveSecret other_secret{};
    other_secret.fill(std::byte{7});
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            auto conn = co_await probe.endpoint->connect(
                asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), server_->port()),
                server_key_->public_key());
            if (!conn) co_return;
            probe.conn = conn.value();
            auto stream = co_await probe.conn->open_stream();
            if (!stream) co_return;
            probe.control = stream.value();
            auto reg = discovery::encode(
                discovery::Register{discovery::rendezvous_group(other_secret).value(),
                                    discovery::rendezvous_group(other_secret).value(),
                                    {}});
            (void)co_await probe.control->write(reg);
            auto ack = co_await wire::read_frame(*probe.control, 64);
            registered = ack && ack.value().type == wire::FrameType::rendezvous_registered;
            auto ask = discovery::encode(discovery::Connect{drive_a_->key().public_key()});
            (void)co_await probe.control->write(ask);
            auto reply = co_await wire::read_frame(*probe.control, 64);
            if (reply && reply.value().type == wire::FrameType::rendezvous_peer_at) {
                answer = discovery::decode_peer_at(reply.value().payload).value();
            }
        },
        asio::detached);
    ASSERT_TRUE(drain([&] { return answer.has_value(); }));
    EXPECT_TRUE(registered);
    EXPECT_FALSE(answer->address) << "the server revealed a member to another group";

    // Relay datagrams to a member of another group are dropped.
    auto dropped = server_->stats().dropped_datagrams;
    // Whatever token is guessed, a member of another group is unreachable.
    transport::RelayToken guess{};
    auto quic_like = random_bytes(100, 3);
    quic_like[0] = std::byte{0x40};
    auto datagram = transport::make_relay(guess, drive_a_->key().public_key(), quic_like);
    probe.endpoint->send_datagram(
        datagram, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), server_->port()));
    EXPECT_TRUE(drain([&] { return server_->stats().dropped_datagrams > dropped; }, 5s));
    probe.conn->close();
    probe.endpoint->close();
}

TEST_F(Rendezvous, UnregisteredSendersCannotRelay) {
    start_pair(true);
    auto key = core::DeviceKey::generate().value();
    auto endpoint = transport::Endpoint::create(io_.get_executor(), key, {}).value();
    auto dropped = server_->stats().dropped_datagrams;
    // A sender that never registered has no token; guessing one gets nothing relayed.
    transport::RelayToken guess{};
    auto quic_like = random_bytes(100, 4);
    quic_like[0] = std::byte{0x40};
    auto datagram = transport::make_relay(guess, drive_a_->key().public_key(), quic_like);
    endpoint->send_datagram(
        datagram, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), server_->port()));
    EXPECT_TRUE(drain([&] { return server_->stats().dropped_datagrams > dropped; }, 5s));
    endpoint->close();
}

TEST_F(Rendezvous, TheServerAnswersNewInitialsWithRetry) {
    // A client that cannot receive at its address never completes a handshake, so spoofed
    // Initials create no server state; a real client follows the Retry transparently.
    auto key = core::DeviceKey::generate().value();
    auto endpoint = transport::Endpoint::create(io_.get_executor(), key, {}).value();
    std::optional<bool> ok;
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            auto conn = co_await endpoint->connect(
                asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), server_->port()),
                server_key_->public_key());
            ok = conn.ok();
            if (conn) conn.value()->close();
        },
        asio::detached);
    ASSERT_TRUE(drain([&] { return ok.has_value(); }));
    EXPECT_TRUE(*ok) << "a real client must get through the Retry";
    endpoint->close();
}

TEST_F(Rendezvous, AFullEarlierGroupDoesNotKeepMembersOut) {
    // Anyone who knows an earlier key can fill its group; members only look in it.
    rendezvous::ServerOptions options;
    options.listen = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0);
    options.max_group_size = 1;
    auto key = core::DeviceKey::generate().value();
    auto server = rendezvous::Server::start(io_.get_executor(), key, options).value();
    core::DriveSecret earlier{};
    earlier.fill(std::byte{1});
    core::DriveSecret current{};
    current.fill(std::byte{2});
    auto group_of = [](const core::DriveSecret& s) {
        return discovery::rendezvous_group(s).value();
    };

    std::vector<std::shared_ptr<transport::Endpoint>> endpoints;
    auto try_register = [&](const discovery::Register& reg) {
        auto client_key = core::DeviceKey::generate().value();
        endpoints.push_back(
            transport::Endpoint::create(io_.get_executor(), client_key, {}).value());
        auto endpoint = endpoints.back();
        std::optional<bool> registered;
        asio::co_spawn(
            io_,
            [&, endpoint]() -> asio::awaitable<void> {
                auto conn = co_await endpoint->connect(
                    asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), server->port()),
                    key.public_key());
                if (!conn) {
                    registered = false;
                    co_return;
                }
                auto stream = co_await conn.value()->open_stream();
                (void)co_await stream.value()->write(discovery::encode(reg));
                auto ack = co_await wire::read_frame(*stream.value(), 64);
                registered = ack && ack.value().type == wire::FrameType::rendezvous_registered;
            },
            asio::detached);
        drain([&] { return registered.has_value(); });
        return registered.value_or(false);
    };
    auto admission = group_of(earlier);
    EXPECT_TRUE(try_register({group_of(earlier), admission, {}}));
    EXPECT_FALSE(try_register({group_of(earlier), admission, {}})) << "the group is full";
    EXPECT_TRUE(try_register({group_of(current), admission, {group_of(earlier)}}));
    for (auto& e : endpoints) e->close();
    server->stop();
}

TEST_F(Rendezvous, AllowListedServersServeOnlyTheirGroups) {
    rendezvous::ServerOptions options;
    options.listen = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0);
    core::DriveSecret mine{};
    mine.fill(std::byte{1});
    options.allowed_groups.insert(discovery::rendezvous_group(mine).value());
    auto key = core::DeviceKey::generate().value();
    auto server = rendezvous::Server::start(io_.get_executor(), key, options).value();

    // The list holds the group of the drive secret; members register in the group of their
    // current discovery key and give the drive secret's group as their admission.
    auto try_register = [&](const core::DriveSecret& current, const core::DriveSecret& secret) {
        auto client_key = core::DeviceKey::generate().value();
        auto endpoint = transport::Endpoint::create(io_.get_executor(), client_key, {}).value();
        std::optional<bool> registered;
        asio::co_spawn(
            io_,
            [&]() -> asio::awaitable<void> {
                auto conn = co_await endpoint->connect(
                    asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), server->port()),
                    key.public_key());
                if (!conn) {
                    registered = false;
                    co_return;
                }
                auto stream = co_await conn.value()->open_stream();
                auto reg = discovery::encode(
                    discovery::Register{discovery::rendezvous_group(current).value(),
                                        discovery::rendezvous_group(secret).value(),
                                        {}});
                (void)co_await stream.value()->write(reg);
                auto ack = co_await wire::read_frame(*stream.value(), 64);
                registered = ack && ack.value().type == wire::FrameType::rendezvous_registered;
                conn.value()->close();
            },
            asio::detached);
        drain([&] { return registered.has_value(); });
        endpoint->close();
        return registered.value_or(false);
    };
    core::DriveSecret theirs{};
    theirs.fill(std::byte{2});
    core::DriveSecret rotated{};
    rotated.fill(std::byte{3});
    EXPECT_TRUE(try_register(mine, mine));
    EXPECT_TRUE(try_register(rotated, mine));
    EXPECT_FALSE(try_register(theirs, theirs));
    EXPECT_FALSE(try_register(mine, theirs)) << "admission, not the group, is checked";
    server->stop();
}

}  // namespace
}  // namespace ghostdrive::daemon
