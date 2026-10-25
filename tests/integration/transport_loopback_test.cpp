// Two endpoints in one process on 127.0.0.1, optionally behind the seeded impairment shim.

#include <gtest/gtest.h>

#include <algorithm>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>

#include "ghostdrive/core/chunk_store.hpp"
#include "ghostdrive/transport/demux.hpp"
#include "ghostdrive/transport/endpoint.hpp"
#include "ghostdrive/transport/frame.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::transport {
namespace {

using namespace std::chrono_literals;
using asio::awaitable;

// ASSERT_* cannot be used in coroutines (it expands to `return;`).
#define CO_ASSERT_TRUE(cond)                      \
    do {                                          \
        if (!(cond)) {                            \
            ADD_FAILURE() << "assertion: " #cond; \
            co_return;                            \
        }                                         \
    } while (0)

#define CO_ASSERT_OK(result)                                                     \
    do {                                                                         \
        if (!(result)) {                                                         \
            ADD_FAILURE() << #result << " failed: " << (result).error().message; \
            co_return;                                                           \
        }                                                                        \
    } while (0)

core::Bytes random_payload(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    core::Bytes data(size);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    return data;
}

awaitable<void> sleep_for(std::chrono::milliseconds duration) {
    asio::steady_timer timer(co_await asio::this_coro::executor, duration);
    co_await timer.async_wait(asio::use_awaitable);
}

// Reads until end of stream.
awaitable<Result<core::Bytes>> read_all(Stream& stream) {
    core::Bytes out;
    std::array<std::byte, 16384> buffer;
    for (;;) {
        auto n = co_await stream.read(buffer);
        if (!n) co_return n.error();
        if (n.value() == 0) co_return out;
        out.insert(out.end(), buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(n.value()));
    }
}

class Loopback : public ::testing::Test {
protected:
    struct Peer {
        core::DeviceKey key;
        std::shared_ptr<Endpoint> endpoint;
    };

    Peer make_peer(std::set<core::PublicKey> pinned, Impairment impairment = {},
                   std::chrono::milliseconds idle = 30s,
                   std::chrono::milliseconds handshake = 10s) {
        auto key = core::DeviceKey::generate();
        EXPECT_TRUE(key);
        EndpointOptions options;
        options.accept_peer = [pinned](const core::PublicKey& k) { return pinned.contains(k); };
        options.impairment = impairment;
        options.idle_timeout = idle;
        options.handshake_timeout = handshake;
        auto endpoint = Endpoint::create(io_.get_executor(), key.value(), options);
        EXPECT_TRUE(endpoint) << endpoint.error().message;
        return Peer{std::move(key).value(), std::move(endpoint).value()};
    }

    static core::DeviceKey new_key() { return std::move(core::DeviceKey::generate()).value(); }

    using Task = std::function<awaitable<void>()>;

    // Runs coroutines to completion on the test's io_context, failing on timeout. The callables
    // stay alive until then, so coroutine lambdas may capture by reference.
    void run(std::vector<Task> tasks, std::chrono::seconds limit = 60s) {
        std::size_t remaining = tasks.size();
        std::exception_ptr error;
        for (auto& task : tasks) {
            asio::co_spawn(io_, task(), [&](std::exception_ptr e) {
                if (e && !error) error = e;
                --remaining;
            });
        }
        auto deadline = std::chrono::steady_clock::now() + limit;
        while (remaining > 0 && std::chrono::steady_clock::now() < deadline) {
            io_.run_one_for(100ms);
        }
        ASSERT_EQ(remaining, 0u) << "timed out";
        if (error) std::rethrow_exception(error);
    }
    void run(Task task, std::chrono::seconds limit = 60s) {
        run(std::vector<Task>{std::move(task)}, limit);
    }

    void TearDown() override {
        // Let close packets and posted completions drain.
        for (auto* ep : endpoints_) ep->close();
        io_.run_for(50ms);
    }

    void track(Peer& peer) { endpoints_.push_back(peer.endpoint.get()); }

    asio::io_context io_;
    std::vector<Endpoint*> endpoints_;
};

awaitable<void> echo_stream(std::shared_ptr<Stream> stream) {
    auto data = co_await read_all(*stream);
    CO_ASSERT_OK(data);
    auto st = co_await stream->write(data.value(), true);
    CO_ASSERT_OK(st);
}

// Server side of an echo: accepts one connection, echoes `streams` streams, and returns once
// every echo has been acknowledged.
awaitable<void> echo_server(std::shared_ptr<Endpoint> endpoint, std::size_t streams) {
    auto conn = co_await endpoint->accept();
    CO_ASSERT_OK(conn);
    auto executor = co_await asio::this_coro::executor;
    std::size_t running = 0;
    for (std::size_t i = 0; i < streams; ++i) {
        auto stream = co_await conn.value()->accept_stream();
        CO_ASSERT_OK(stream);
        ++running;
        asio::co_spawn(executor, echo_stream(stream.value()),
                       [&running](std::exception_ptr) { --running; });
    }
    while (running > 0) co_await sleep_for(5ms);
}

awaitable<void> echo_once(std::shared_ptr<Connection> conn, core::Bytes payload) {
    auto stream = co_await conn->open_stream();
    CO_ASSERT_OK(stream);
    auto st = co_await stream.value()->write(payload, true);
    CO_ASSERT_OK(st);
    auto echoed = co_await read_all(*stream.value());
    CO_ASSERT_OK(echoed);
    EXPECT_TRUE(echoed.value() == payload)
        << "echo mismatch, got " << echoed.value().size() << " of " << payload.size() << " bytes";
}

TEST_F(Loopback, HandshakeAuthenticatesBothPeers) {
    auto client = make_peer({});
    track(client);
    auto server = make_peer({client.key.public_key()});
    track(server);

    run([&]() -> awaitable<void> {
        auto accepted = server.endpoint->accept();
        auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                      server.key.public_key());
        CO_ASSERT_OK(conn);
        EXPECT_EQ(conn.value()->peer_key(), server.key.public_key());
        auto incoming = co_await std::move(accepted);
        CO_ASSERT_OK(incoming);
        EXPECT_EQ(incoming.value()->peer_key(), client.key.public_key());
    });
}

TEST_F(Loopback, ClientRejectsServerWithUnexpectedKey) {
    auto client = make_peer({});
    track(client);
    auto server = make_peer({client.key.public_key()});
    track(server);
    auto impostor_expected = new_key().public_key();

    run([&]() -> awaitable<void> {
        auto conn =
            co_await client.endpoint->connect(server.endpoint->local_endpoint(), impostor_expected);
        CO_ASSERT_TRUE(!conn);
        EXPECT_NE(conn.error().code, core::Errc::timeout) << conn.error().message;
    });
}

TEST_F(Loopback, ServerRejectsUnpinnedClient) {
    auto client = make_peer({});
    track(client);
    auto server = make_peer({new_key().public_key()});  // pins someone else
    track(server);

    bool server_accepted = false;
    asio::co_spawn(
        io_,
        [&]() -> awaitable<void> {
            auto conn = co_await server.endpoint->accept();
            server_accepted = conn.ok();
        },
        asio::detached);

    run([&]() -> awaitable<void> {
        auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                      server.key.public_key());
        // The server rejects the client certificate, so the handshake is never confirmed.
        CO_ASSERT_TRUE(!conn);
        EXPECT_NE(conn.error().code, core::Errc::timeout) << conn.error().message;
    });
    EXPECT_FALSE(server_accepted);
}

TEST_F(Loopback, ServerWithoutPinsAcceptsNobody) {
    auto client = make_peer({}, {}, 30s, 1s);
    track(client);
    auto key = new_key();
    EndpointOptions options;  // accept_peer left empty
    auto server = Endpoint::create(io_.get_executor(), key, options);
    ASSERT_TRUE(server);
    endpoints_.push_back(server.value().get());

    run(
        [&]() -> awaitable<void> {
            auto conn = co_await client.endpoint->connect(server.value()->local_endpoint(),
                                                          key.public_key());
            CO_ASSERT_TRUE(!conn);
            // The server never answers, so the client's handshake runs out of time.
            EXPECT_EQ(conn.error().code, core::Errc::timeout) << conn.error().message;
        },
        20s);
}

TEST_F(Loopback, HandshakeLimitDropsNewInitials) {
    auto client = make_peer({}, {}, 30s, 1s);
    track(client);
    auto key = new_key();
    EndpointOptions options;
    options.accept_peer = [pinned = client.key.public_key()](const core::PublicKey& k) {
        return k == pinned;
    };
    options.max_pending_handshakes = 0;
    auto server = Endpoint::create(io_.get_executor(), key, options);
    ASSERT_TRUE(server);
    endpoints_.push_back(server.value().get());

    run(
        [&]() -> awaitable<void> {
            auto conn = co_await client.endpoint->connect(server.value()->local_endpoint(),
                                                          key.public_key());
            CO_ASSERT_TRUE(!conn);
            EXPECT_EQ(conn.error().code, core::Errc::timeout) << conn.error().message;
        },
        20s);
}

TEST_F(Loopback, ThrowingPinCheckRejects) {
    auto client = make_peer({});
    track(client);
    auto key = new_key();
    EndpointOptions options;
    options.accept_peer = [](const core::PublicKey&) -> bool { throw std::runtime_error("boom"); };
    auto server = Endpoint::create(io_.get_executor(), key, options);
    ASSERT_TRUE(server);
    endpoints_.push_back(server.value().get());

    run([&]() -> awaitable<void> {
        auto conn = co_await client.endpoint->connect(server.value()->local_endpoint(),
                                                      key.public_key());
        CO_ASSERT_TRUE(!conn);
        EXPECT_NE(conn.error().code, core::Errc::timeout) << conn.error().message;
    });
}

TEST_F(Loopback, EchoOneStream) {
    auto client = make_peer({});
    track(client);
    auto server = make_peer({client.key.public_key()});
    track(server);

    auto payload = random_payload(100'000, 1);
    std::vector<Task> tasks;
    tasks.push_back([&] { return echo_server(server.endpoint, 1); });
    tasks.push_back([&]() -> awaitable<void> {
        auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                      server.key.public_key());
        CO_ASSERT_OK(conn);
        co_await echo_once(conn.value(), payload);
    });
    run(std::move(tasks));
}

TEST_F(Loopback, ZeroLengthStreamEndsImmediately) {
    auto client = make_peer({});
    track(client);
    auto server = make_peer({client.key.public_key()});
    track(server);

    std::vector<Task> tasks;
    tasks.push_back([&] { return echo_server(server.endpoint, 1); });
    tasks.push_back([&]() -> awaitable<void> {
        auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                      server.key.public_key());
        CO_ASSERT_OK(conn);
        co_await echo_once(conn.value(), {});
    });
    run(std::move(tasks));
}

TEST_F(Loopback, LargeTransferSurvivesLatencyLossAndReordering) {
    Impairment impairment;
    impairment.delay = 10ms;
    impairment.jitter = 2ms;
    impairment.loss = 0.02;
    impairment.reorder = 0.05;
    impairment.reorder_delay = 5ms;
    impairment.duplicate = 0.01;

    impairment.seed = 11;
    auto client = make_peer({}, impairment);
    track(client);
    impairment.seed = 12;
    auto server = make_peer({client.key.public_key()}, impairment);
    track(server);

    auto payload = random_payload(4 * 1024 * 1024, 2);
    std::vector<Task> tasks;
    tasks.push_back([&] { return echo_server(server.endpoint, 1); });
    tasks.push_back([&]() -> awaitable<void> {
        auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                      server.key.public_key());
        CO_ASSERT_OK(conn);
        co_await echo_once(conn.value(), payload);
    });
    run(std::move(tasks), 90s);
}

TEST_F(Loopback, ManyConcurrentStreamsUnderLoss) {
    Impairment impairment;
    impairment.delay = 2ms;
    impairment.loss = 0.01;
    impairment.seed = 21;
    auto client = make_peer({}, impairment);
    track(client);
    impairment.seed = 22;
    auto server = make_peer({client.key.public_key()}, impairment);
    track(server);

    // More than the peer's initial stream limit of 128, so open_stream must wait for credit.
    constexpr std::size_t kStreams = 200;
    std::vector<Task> tasks;
    tasks.push_back([&] { return echo_server(server.endpoint, kStreams); });
    tasks.push_back([&]() -> awaitable<void> {
        auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                      server.key.public_key());
        CO_ASSERT_OK(conn);
        std::size_t remaining = kStreams;
        auto executor = co_await asio::this_coro::executor;
        for (std::size_t i = 0; i < kStreams; ++i) {
            asio::co_spawn(executor, echo_once(conn.value(), random_payload(16 * 1024, 100 + i)),
                           [&remaining](std::exception_ptr) { --remaining; });
        }
        while (remaining > 0) co_await sleep_for(10ms);
    });
    run(std::move(tasks), 90s);
}

TEST_F(Loopback, GetChunkOverTheWire) {
    test::TempDir dir;
    auto secret = core::generate_drive_secret();
    ASSERT_TRUE(secret);
    auto server_store =
        core::ChunkStore::open(dir.path() / "server", core::ChunkHasher(secret.value()));
    ASSERT_TRUE(server_store);
    auto client_store =
        core::ChunkStore::open(dir.path() / "client", core::ChunkHasher(secret.value()));
    ASSERT_TRUE(client_store);

    auto chunk = random_payload(1024 * 1024, 3);
    auto id = server_store.value().put(chunk);
    ASSERT_TRUE(id);
    auto missing = server_store.value().hasher().hash(random_payload(10, 4));

    auto client = make_peer({});
    track(client);
    auto server = make_peer({client.key.public_key()});
    track(server);

    auto serve = [&]() -> awaitable<void> {
        auto conn = co_await server.endpoint->accept();
        CO_ASSERT_OK(conn);
        for (int i = 0; i < 2; ++i) {
            auto stream = co_await conn.value()->accept_stream();
            CO_ASSERT_OK(stream);
            auto& s = *stream.value();
            auto request = co_await wire::read_frame(s);
            CO_ASSERT_OK(request);
            CO_ASSERT_TRUE(request.value().type == wire::FrameType::get_chunk);
            auto wanted = wire::decode_get_chunk(request.value().payload);
            CO_ASSERT_OK(wanted);
            auto data = server_store.value().read_verified(wanted.value());
            if (!data) {
                auto reply = wire::encode_error(wire::ErrorCode::not_found);
                CO_ASSERT_OK(co_await s.write(reply, true));
                continue;
            }
            auto header = wire::encode_header(wire::FrameType::chunk_data,
                                              static_cast<std::uint32_t>(data.value().size()));
            const std::span<const std::byte> parts[] = {header, data.value()};
            CO_ASSERT_OK(co_await s.write(parts, true));
        }
    };

    auto fetch = [&]() -> awaitable<void> {
        auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                      server.key.public_key());
        CO_ASSERT_OK(conn);

        auto stream = co_await conn.value()->open_stream();
        CO_ASSERT_OK(stream);
        auto request = wire::encode_get_chunk(id.value());
        CO_ASSERT_OK(co_await stream.value()->write(request, true));
        auto reply = co_await wire::read_frame(*stream.value());
        CO_ASSERT_OK(reply);
        CO_ASSERT_TRUE(reply.value().type == wire::FrameType::chunk_data);
        // Every fetched chunk is hash-verified before it is stored.
        CO_ASSERT_OK(client_store.value().put_verified(id.value(), reply.value().payload));
        EXPECT_TRUE(client_store.value().contains(id.value()));

        auto stream2 = co_await conn.value()->open_stream();
        CO_ASSERT_OK(stream2);
        auto request2 = wire::encode_get_chunk(missing);
        CO_ASSERT_OK(co_await stream2.value()->write(request2, true));
        auto reply2 = co_await wire::read_frame(*stream2.value());
        CO_ASSERT_OK(reply2);
        CO_ASSERT_TRUE(reply2.value().type == wire::FrameType::error);
        auto code = wire::decode_error(reply2.value().payload);
        CO_ASSERT_OK(code);
        EXPECT_EQ(code.value(), wire::ErrorCode::not_found);
    };

    std::vector<Task> tasks;
    tasks.push_back(serve);
    tasks.push_back(fetch);
    run(std::move(tasks));
}

TEST_F(Loopback, PeerCloseFailsPendingRead) {
    auto client = make_peer({});
    track(client);
    auto server = make_peer({client.key.public_key()});
    track(server);

    std::vector<Task> tasks;
    tasks.push_back([&]() -> awaitable<void> {
        auto conn = co_await server.endpoint->accept();
        CO_ASSERT_OK(conn);
        auto stream = co_await conn.value()->accept_stream();
        CO_ASSERT_OK(stream);
        std::array<std::byte, 1> ping;
        CO_ASSERT_OK(co_await wire::read_exact(*stream.value(), ping));
        // Give the client time to see its write acknowledged and start reading.
        co_await sleep_for(100ms);
        conn.value()->close(7);
    });
    tasks.push_back([&]() -> awaitable<void> {
        auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                      server.key.public_key());
        CO_ASSERT_OK(conn);
        auto stream = co_await conn.value()->open_stream();
        CO_ASSERT_OK(stream);
        const std::byte ping[] = {std::byte{1}};
        CO_ASSERT_OK(co_await stream.value()->write(ping));
        std::array<std::byte, 16> buffer;
        auto n = co_await stream.value()->read(buffer);
        CO_ASSERT_TRUE(!n);
        EXPECT_EQ(n.error().code, core::Errc::closed) << n.error().message;
        EXPECT_FALSE(conn.value()->is_open());
    });
    run(std::move(tasks), 20s);
}

TEST_F(Loopback, IdleConnectionTimesOut) {
    auto client = make_peer({}, {}, 300ms);
    track(client);
    auto server = make_peer({client.key.public_key()}, {}, 300ms);
    track(server);

    run(
        [&]() -> awaitable<void> {
            auto conn = co_await client.endpoint->connect(server.endpoint->local_endpoint(),
                                                          server.key.public_key());
            CO_ASSERT_OK(conn);
            auto stream = co_await conn.value()->accept_stream();
            CO_ASSERT_TRUE(!stream);
            EXPECT_EQ(stream.error().code, core::Errc::timeout) << stream.error().message;
        },
        20s);
}

TEST_F(Loopback, MagicDatagramsReachTheDatagramHandler) {
    auto a = make_peer({});
    track(a);
    auto b = make_peer({});
    track(b);

    core::Bytes received;
    b.endpoint->set_datagram_handler(
        [&](std::span<const std::byte> datagram, const Udp::endpoint&) {
            received.assign(datagram.begin(), datagram.end());
        });
    core::Bytes probe(kDatagramMagic.begin(), kDatagramMagic.end());
    probe.push_back(std::byte{0x99});

    run([&]() -> awaitable<void> {
        a.endpoint->send_datagram(probe, b.endpoint->local_endpoint());
        for (int i = 0; i < 100 && received.empty(); ++i) co_await sleep_for(10ms);
        EXPECT_TRUE(received == probe);
    });
}

TEST_F(Loopback, DatagramFramesFlowOnlyWhenBothSidesEnableThem) {
    auto client_key = new_key();
    auto client = Endpoint::create(io_.get_executor(), client_key, {}).value();
    auto server_key = new_key();
    EndpointOptions options;
    auto client_public = client_key.public_key();
    options.accept_peer = [client_public](const core::PublicKey& k) { return k == client_public; };
    options.accept_datagrams = true;
    auto server = Endpoint::create(io_.get_executor(), server_key, options).value();
    endpoints_.push_back(client.get());
    endpoints_.push_back(server.get());

    // The largest frame must fit in one packet of the connection.
    auto big = random_payload(Connection::kMaxDatagram, 7);
    run([&]() -> awaitable<void> {
        auto plain = co_await client->connect(server->local_endpoint(), server_key.public_key());
        CO_ASSERT_OK(plain);
        auto plain_accepted = co_await server->accept();
        CO_ASSERT_OK(plain_accepted);
        EXPECT_FALSE(plain.value()->send_datagram(core::Bytes(10)))
            << "a connection made without datagrams sent one";

        auto conn = co_await client->connect(server->local_endpoint(), server_key.public_key(),
                                             {.datagrams = true});
        CO_ASSERT_OK(conn);
        auto accepted = co_await server->accept();
        CO_ASSERT_OK(accepted);
        core::Bytes at_server;
        accepted.value()->set_datagram_handler([&](core::Bytes d) { at_server = std::move(d); });
        core::Bytes at_client;
        conn.value()->set_datagram_handler([&](core::Bytes d) { at_client = std::move(d); });
        core::Bytes at_plain;
        plain.value()->set_datagram_handler([&](core::Bytes d) { at_plain = std::move(d); });

        EXPECT_FALSE(conn.value()->send_datagram(core::Bytes(Connection::kMaxDatagram + 1)));
        // Datagrams are unreliable; resend until one arrives.
        for (int i = 0; i < 50 && at_server.empty(); ++i) {
            EXPECT_TRUE(conn.value()->send_datagram(big));
            co_await sleep_for(20ms);
        }
        EXPECT_TRUE(at_server == big);
        for (int i = 0; i < 50 && at_client.empty(); ++i) {
            EXPECT_TRUE(accepted.value()->send_datagram(big));
            // The peer of this one never enabled datagrams, so the frame is discarded unsent.
            (void)plain_accepted.value()->send_datagram(big);
            co_await sleep_for(20ms);
        }
        EXPECT_TRUE(at_client == big);
        EXPECT_TRUE(at_plain.empty());
        EXPECT_TRUE(plain.value()->is_open() && plain_accepted.value()->is_open());
        // The handlers refer to this frame's locals.
        for (auto* c : {accepted.value().get(), conn.value().get(), plain.value().get()}) {
            c->set_datagram_handler(nullptr);
        }
    });
}

TEST_F(Loopback, AFloodOfDatagramsDoesNotStarveStreams) {
    auto client_key = new_key();
    auto client = Endpoint::create(io_.get_executor(), client_key, {}).value();
    auto server_key = new_key();
    EndpointOptions options;
    auto client_public = client_key.public_key();
    options.accept_peer = [client_public](const core::PublicKey& k) { return k == client_public; };
    options.accept_datagrams = true;
    auto server = Endpoint::create(io_.get_executor(), server_key, options).value();
    endpoints_.push_back(client.get());
    endpoints_.push_back(server.get());

    auto payload = random_payload(512 * 1024, 8);
    // Shared with the flooding coroutine, which may outlive this test if the echo stalls.
    struct Flood {
        core::Bytes frame = random_payload(Connection::kMaxDatagram, 9);
        std::size_t queued = 0;
        bool done = false;
    };
    auto flood = std::make_shared<Flood>();
    bool echoed = false;
    run({[&]() -> awaitable<void> { co_await echo_server(server, 1); },
         [&]() -> awaitable<void> {
             auto conn = co_await client->connect(server->local_endpoint(),
                                                  server_key.public_key(), {.datagrams = true});
             CO_ASSERT_OK(conn);
             // Keeps the frame queue full for as long as the echo runs.
             asio::co_spawn(
                 co_await asio::this_coro::executor,
                 [flood, c = conn.value()]() -> awaitable<void> {
                     while (!flood->done) {
                         while (c->send_datagram(flood->frame)) ++flood->queued;
                         co_await sleep_for(1ms);
                     }
                 },
                 asio::detached);
             co_await echo_once(conn.value(), payload);
             echoed = true;
             flood->done = true;
             co_await sleep_for(5ms);
         }},
        30s);
    flood->done = true;
    EXPECT_TRUE(echoed) << "the stream starved behind DATAGRAM frames";
    // The queue was refilled throughout, so frames really competed with the stream: more were
    // queued than the 64 the queue holds at once, and more than the echo's own packets.
    EXPECT_GT(flood->queued, 2 * payload.size() / Connection::kMaxDatagram) << "the flood was too thin";
}

// A relayed connection must authenticate the key behind its virtual address. A malicious
// rendezvous server relays C's handshake to A but labels it as coming from B, another member.
TEST_F(Loopback, ARelayedConnectionMustBeWithTheKeyOfItsVirtualAddress) {
    auto a_key = new_key();
    auto b_key = new_key();  // never online
    auto c_key = new_key();
    auto a_public = a_key.public_key();
    std::set<core::PublicKey> members{b_key.public_key(), c_key.public_key()};
    EndpointOptions a_options;
    a_options.accept_peer = [members](const core::PublicKey& k) { return members.contains(k); };
    auto a = Endpoint::create(io_.get_executor(), a_key, a_options).value();
    EndpointOptions c_options;
    c_options.handshake_timeout = 3s;
    auto c = Endpoint::create(io_.get_executor(), c_key, c_options).value();
    auto server_key = new_key();
    EndpointOptions server_options;
    std::set<core::PublicKey> clients{a_public, c_key.public_key()};
    server_options.accept_peer = [clients](const core::PublicKey& k) { return clients.contains(k); };
    server_options.accept_datagrams = true;
    auto server = Endpoint::create(io_.get_executor(), server_key, server_options).value();
    for (auto* e : {a.get(), c.get(), server.get()}) endpoints_.push_back(e);

    core::PublicKey claimed{};  // the source key the server puts on C's frames
    std::shared_ptr<Connection> at_a;
    std::shared_ptr<Connection> at_c;
    auto relabel = [](core::Bytes frame, const core::PublicKey& source,
                      const std::shared_ptr<Connection>& to) {
        std::copy(source.begin(), source.end(), frame.begin());
        if (to) to->send_datagram(std::move(frame));
    };
    std::vector<bool> results;
    run([&]() -> awaitable<void> {
        auto to_server = server->local_endpoint();
        auto a_conn = co_await a->connect(to_server, server_key.public_key(), {.datagrams = true});
        CO_ASSERT_OK(a_conn);
        auto first = co_await server->accept();
        CO_ASSERT_OK(first);
        auto c_conn = co_await c->connect(to_server, server_key.public_key(), {.datagrams = true});
        CO_ASSERT_OK(c_conn);
        auto second = co_await server->accept();
        CO_ASSERT_OK(second);
        at_a = first.value()->peer_key() == a_public ? first.value() : second.value();
        at_c = at_a == first.value() ? second.value() : first.value();
        at_c->set_datagram_handler([&](core::Bytes f) { relabel(std::move(f), claimed, at_a); });
        at_a->set_datagram_handler([&](core::Bytes f) { relabel(std::move(f), a_public, at_c); });
        auto any = [](const core::PublicKey&) { return true; };
        a->set_relay(a_conn.value(), any);
        c->set_relay(c_conn.value(), any);

        auto via_relay = *c->relay_address(a_public);
        claimed = b_key.public_key();
        auto forged = co_await c->connect(via_relay, a_public);
        results.push_back(forged.ok());
        claimed = c_key.public_key();
        auto honest = co_await c->connect(via_relay, a_public);
        results.push_back(honest.ok());
        if (honest) honest.value()->close();
        at_a->set_datagram_handler(nullptr);
        at_c->set_datagram_handler(nullptr);
    });
    ASSERT_EQ(results.size(), 2u);
    EXPECT_FALSE(results[0]) << "A accepted C at B's virtual address";
    EXPECT_TRUE(results[1]) << "A refused C at its own virtual address";
}

}  // namespace
}  // namespace ghostdrive::transport
