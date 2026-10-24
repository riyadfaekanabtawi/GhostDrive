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

}  // namespace
}  // namespace ghostdrive::transport
