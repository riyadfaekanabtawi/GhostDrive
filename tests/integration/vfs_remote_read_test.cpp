// Reads through vfs::Filesystem where the chunks live only on other devices, fetched over real
// QUIC connections on 127.0.0.1.

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>
#include <fstream>
#include <map>
#include <random>

#include "ghostdrive/core/chunk_store.hpp"
#include "ghostdrive/core/chunking.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "ghostdrive/core/metadata_index.hpp"
#include "ghostdrive/transport/endpoint.hpp"
#include "ghostdrive/transport/frame.hpp"
#include "ghostdrive/vfs/chunk_exchange.hpp"
#include "ghostdrive/vfs/filesystem.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::vfs {
namespace {

using namespace std::chrono_literals;
namespace wire = transport::wire;

core::Bytes random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    core::Bytes data(size);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    return data;
}

// A peer that answers every GetChunk with bytes of the right size but the wrong content.
asio::awaitable<void> serve_garbage(std::shared_ptr<transport::Connection> conn,
                                    const std::map<core::ChunkId, std::uint32_t>& sizes) {
    for (;;) {
        auto stream = co_await conn->accept_stream();
        if (!stream) co_return;
        auto request = co_await wire::read_frame(*stream.value(), 64);
        if (!request) continue;
        auto id = wire::decode_get_chunk(request.value().payload);
        if (!id) continue;
        auto it = sizes.find(id.value());
        if (it == sizes.end()) continue;
        auto garbage = random_bytes(it->second, 99);
        auto header = wire::encode_header(wire::FrameType::chunk_data,
                                          static_cast<std::uint32_t>(garbage.size()));
        const std::span<const std::byte> parts[] = {header, garbage};
        (void)co_await stream.value()->write(parts, true);
    }
}

// A peer that accepts requests and never answers.
asio::awaitable<void> serve_nothing(std::shared_ptr<transport::Connection> conn) {
    std::vector<std::shared_ptr<transport::Stream>> held;
    for (;;) {
        auto stream = co_await conn->accept_stream();
        if (!stream) co_return;
        held.push_back(std::move(stream).value());
    }
}

class RemoteRead : public ::testing::Test {
protected:
    enum class Behavior { honest, garbage, silent };

    struct Server {
        core::DeviceKey key;
        std::shared_ptr<transport::Endpoint> endpoint;
        std::unique_ptr<core::ChunkStore> store;
    };

    RemoteRead()
        : secret_(core::generate_drive_secret().value()),
          client_key_(core::DeviceKey::generate().value()),
          client_store_(
              core::ChunkStore::open(dir_.path() / "client-store", core::ChunkHasher(secret_))
                  .value()),
          index_(core::MetadataIndex::open(dir_.path() / "client.db", core::DriveId{}, client_key_)
                     .value()),
          cache_(256 * 1024 * 1024) {}

    ~RemoteRead() override {
        for (auto& server : servers_) server->endpoint->close();
        if (client_) client_->close();
        io_.run_for(50ms);
        disk_.join();
    }

    void start_client(transport::Impairment impairment = {}) {
        transport::EndpointOptions options;
        options.impairment = impairment;
        client_ = transport::Endpoint::create(io_.get_executor(), client_key_, options).value();
        fetcher_ = std::make_unique<Fetcher>(
            io_.get_executor(), disk_.get_executor(), client_store_, index_, cache_,
            [this](const core::PublicKey& key) -> std::shared_ptr<transport::Connection> {
                auto it = connections_.find(key);
                return it == connections_.end() ? nullptr : it->second;
            },
            per_request_timeout_);
        FilesystemOptions fs_options;
        fs_options.request_timeout = request_timeout_;
        fs_ = std::make_unique<Filesystem>(io_.get_executor(), tree_, *fetcher_, fs_options);
    }

    Server& add_server(Behavior behavior, transport::Impairment impairment = {},
                       std::optional<core::DeviceKey> key = std::nullopt) {
        auto server = std::make_unique<Server>(Server{
            key ? std::move(*key) : core::DeviceKey::generate().value(), nullptr,
            std::make_unique<core::ChunkStore>(
                core::ChunkStore::open(dir_.path() / ("server" + std::to_string(servers_.size())),
                                       core::ChunkHasher(secret_))
                    .value())});
        transport::EndpointOptions options;
        options.accept_peer = [pin = client_key_.public_key()](const core::PublicKey& k) {
            return k == pin;
        };
        options.impairment = impairment;
        server->endpoint =
            transport::Endpoint::create(io_.get_executor(), server->key, options).value();

        auto* raw = server.get();
        asio::co_spawn(
            io_,
            [this, raw, behavior]() -> asio::awaitable<void> {
                for (;;) {
                    auto conn = co_await raw->endpoint->accept();
                    if (!conn) co_return;
                    auto executor = co_await asio::this_coro::executor;
                    switch (behavior) {
                        case Behavior::honest:
                            asio::co_spawn(
                                executor,
                                serve_chunks(conn.value(), *raw->store, disk_.get_executor()),
                                asio::detached);
                            break;
                        case Behavior::garbage:
                            asio::co_spawn(executor, serve_garbage(conn.value(), sizes_),
                                           asio::detached);
                            break;
                        case Behavior::silent:
                            asio::co_spawn(executor, serve_nothing(conn.value()), asio::detached);
                            break;
                    }
                }
            },
            asio::detached);
        servers_.push_back(std::move(server));
        return *servers_.back();
    }

    void connect(Server& server) {
        std::optional<core::Result<std::shared_ptr<transport::Connection>>> conn;
        asio::co_spawn(
            io_,
            [&]() -> asio::awaitable<void> {
                conn = co_await client_->connect(server.endpoint->local_endpoint(),
                                                 server.key.public_key());
            },
            asio::detached);
        drain([&] { return conn.has_value(); }, 10s);
        ASSERT_TRUE(*conn) << conn->error().message;
        connections_[server.key.public_key()] = conn->value();
    }

    // Publishes a file whose chunks live only in `holders` (listed in that order in the index).
    core::FileRecord publish(const std::string& path, const core::Bytes& data,
                             std::vector<Server*> stores, std::vector<Server*> holders) {
        auto source = dir_.path() / ("src-" + std::to_string(counter_++));
        std::ofstream(source, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        // The origin store only computes chunk IDs; it is never served from.
        auto origin = core::ChunkStore::open(dir_.path() / ("origin-" + std::to_string(counter_)),
                                             core::ChunkHasher(secret_));
        auto imported = origin.value().import_file(source).value();
        for (auto* s : stores) EXPECT_TRUE(s->store->import_file(source));

        core::FileRecord record;
        record.path = path;
        record.size = imported.size;
        record.mode = 0644;
        record.block_size = imported.block_size;
        record.chunks = imported.chunks;
        for (auto* h : holders) {
            EXPECT_TRUE(index_.add_device(h->key.public_key(), "server"));
        }
        auto committed = index_.commit_local(record, clock_);
        EXPECT_TRUE(committed) << committed.error().message;
        for (std::size_t i = 0; i < record.chunks.size(); ++i) {
            sizes_[record.chunks[i]] = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(record.block_size, record.size - i * record.block_size));
            for (auto* h : holders) {
                EXPECT_TRUE(index_.set_holds(h->key.public_key(), record.chunks[i], true));
            }
        }
        EXPECT_TRUE(tree_.refresh(index_));
        return record;
    }

    core::Result<core::Bytes> read_file(const std::string& path, std::size_t read_size) {
        auto ino = tree_.resolve(path);
        if (!ino) return core::Error{core::Errc::not_found, "not in tree"};
        auto handle = fs_->open(*ino);
        if (!handle) return handle.error();
        auto size = tree_.getattr(*ino)->size;
        core::Bytes out;
        for (std::uint64_t offset = 0; offset < size; offset += read_size) {
            std::optional<core::Result<ReadReply>> result;
            fs_->read(handle.value(), offset, read_size,
                      [&](core::Result<ReadReply> r) { result = std::move(r); });
            drain([&] { return result.has_value(); }, 60s);
            if (!result) return core::Error{core::Errc::timeout, "test drain timed out"};
            if (!*result) {
                fs_->release(handle.value());
                return result->error();
            }
            for (auto s : result->value().slices) out.insert(out.end(), s.begin(), s.end());
        }
        fs_->release(handle.value());
        return out;
    }

    template <typename Done>
    void drain(Done done, std::chrono::seconds limit) {
        auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < deadline) io_.run_one_for(10ms);
    }

    test::TempDir dir_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_{io_.get_executor()};
    asio::thread_pool disk_{2};
    core::DriveSecret secret_;
    core::DeviceKey client_key_;
    core::ChunkStore client_store_;
    core::MetadataIndex index_;
    core::HybridClock clock_;
    core::ChunkCache cache_;
    Tree tree_;
    std::chrono::milliseconds request_timeout_{30'000};
    std::chrono::milliseconds per_request_timeout_{10'000};
    std::shared_ptr<transport::Endpoint> client_;
    std::unique_ptr<Fetcher> fetcher_;
    std::unique_ptr<Filesystem> fs_;
    std::vector<std::unique_ptr<Server>> servers_;
    std::map<core::PublicKey, std::shared_ptr<transport::Connection>> connections_;
    std::map<core::ChunkId, std::uint32_t> sizes_;
    int counter_ = 0;
};

TEST_F(RemoteRead, StreamsAFileThatLivesOnlyOnAPeer) {
    transport::Impairment impairment;
    impairment.delay = 5ms;
    impairment.loss = 0.01;
    impairment.seed = 31;
    start_client(impairment);
    impairment.seed = 32;
    auto& server = add_server(Behavior::honest, impairment);
    connect(server);

    // 3 MiB at the 128 KiB block size: 24 chunks, none of them on the client.
    auto data = random_bytes(3 * 1024 * 1024 + 4321, 1);
    auto record = publish("movies/clip.mkv", data, {&server}, {&server});
    for (const auto& id : record.chunks) ASSERT_FALSE(client_store_.contains(id));

    auto got = read_file("movies/clip.mkv", 32 * 1024);  // FUSE-T's default NFS read size
    ASSERT_TRUE(got) << got.error().message;
    EXPECT_TRUE(got.value() == data);
    EXPECT_EQ(fetcher_->stats().remote_fetches, record.chunks.size());
    EXPECT_EQ(fetcher_->stats().hash_mismatches, 0u);
    EXPECT_GT(fetcher_->stats().cache_hits, 0u);
}

TEST_F(RemoteRead, RandomAccessFetchesOnlyTheChunksItNeeds) {
    start_client();
    auto& server = add_server(Behavior::honest);
    connect(server);
    auto data = random_bytes(40 * core::kMinBlockSize, 2);
    auto record = publish("big.bin", data, {&server}, {&server});

    auto ino = tree_.resolve("big.bin");
    auto handle = fs_->open(*ino);
    ASSERT_TRUE(handle);
    std::uint64_t offset = 30 * std::uint64_t{core::kMinBlockSize} + 777;
    std::optional<core::Result<ReadReply>> result;
    fs_->read(handle.value(), offset, 4096,
              [&](core::Result<ReadReply> r) { result = std::move(r); });
    drain([&] { return result.has_value(); }, 20s);
    ASSERT_TRUE(result && *result);
    core::Bytes got;
    for (auto s : result->value().slices) got.insert(got.end(), s.begin(), s.end());
    EXPECT_TRUE(got == core::Bytes(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                   data.begin() + static_cast<std::ptrdiff_t>(offset) + 4096));
    EXPECT_EQ(fetcher_->stats().remote_fetches, 1u);  // a seek is not sequential: no readahead
    fs_->release(handle.value());
}

TEST_F(RemoteRead, PeerSendingWrongBytesIsSkipped) {
    start_client();
    // Holders are tried in device key order, so the liar gets the smaller key and is asked first.
    auto k1 = core::DeviceKey::generate().value();
    auto k2 = core::DeviceKey::generate().value();
    if (k2.public_key() < k1.public_key()) std::swap(k1, k2);
    auto& liar = add_server(Behavior::garbage, {}, std::move(k1));
    auto& honest = add_server(Behavior::honest, {}, std::move(k2));
    connect(liar);
    connect(honest);
    auto data = random_bytes(2 * core::kMinBlockSize, 3);
    publish("f", data, {&honest}, {&liar, &honest});

    auto got = read_file("f", 64 * 1024);
    ASSERT_TRUE(got) << got.error().message;
    EXPECT_TRUE(got.value() == data);
    EXPECT_GE(fetcher_->stats().hash_mismatches, 1u);
    // Nothing unverified reached the cache or the local store.
    for (const auto& [id, size] : sizes_) EXPECT_FALSE(client_store_.contains(id));
}

TEST_F(RemoteRead, OnlyLiarsMeansTheReadFails) {
    start_client();
    auto& liar = add_server(Behavior::garbage);
    connect(liar);
    auto record = publish("f", random_bytes(1000, 4), {}, {&liar});

    auto got = read_file("f", 4096);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, core::Errc::corrupt) << got.error().message;
    EXPECT_FALSE(cache_.contains(record.chunks[0]));
}

TEST_F(RemoteRead, NoConnectedHolderMeansNotFound) {
    start_client();
    auto& server = add_server(Behavior::honest);  // never connected
    publish("f", random_bytes(1000, 5), {&server}, {&server});

    auto got = read_file("f", 4096);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, core::Errc::not_found) << got.error().message;
}

TEST_F(RemoteRead, StalledPeerHitsTheRequestDeadline) {
    request_timeout_ = 300ms;
    start_client();
    auto& silent = add_server(Behavior::silent);
    connect(silent);
    publish("f", random_bytes(1000, 6), {}, {&silent});

    auto got = read_file("f", 4096);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, core::Errc::timeout) << got.error().message;
}

TEST_F(RemoteRead, PartialWriteToARemoteFileFetchesOnlyTheTouchedBlock) {
    start_client();
    auto& server = add_server(Behavior::honest);
    connect(server);
    auto data = random_bytes(6 * core::kMinBlockSize, 7);
    auto before = publish("remote.bin", data, {&server}, {&server});

    Filesystem writable(io_.get_executor(), tree_, *fetcher_, {},
                        WriteSupport{client_store_, index_, clock_, disk_.get_executor(),
                                     dir_.path() / "staging"});
    auto ino = tree_.resolve("remote.bin");
    ASSERT_TRUE(ino);
    auto handle = writable.open(*ino, true);
    ASSERT_TRUE(handle) << handle.error().message;

    auto patch = random_bytes(10, 8);
    std::uint64_t offset = 4 * std::uint64_t{core::kMinBlockSize} + 50;
    std::optional<core::Status> wrote;
    writable.write(handle.value(), offset, patch, [&](core::Status st) { wrote = st; });
    drain([&] { return wrote.has_value(); }, 20s);
    ASSERT_TRUE(wrote && *wrote);
    std::optional<core::Status> flushed;
    writable.flush(handle.value(), [&](core::Status st) { flushed = st; });
    drain([&] { return flushed.has_value(); }, 20s);
    ASSERT_TRUE(flushed && *flushed) << (flushed ? flushed->error().message : "timeout");
    writable.release(handle.value());

    // Only block 4 crossed the network (to be patched); the rest kept their remote chunk IDs.
    EXPECT_EQ(fetcher_->stats().remote_fetches, 1u);
    auto after = index_.get("remote.bin").value().value();
    ASSERT_EQ(after.chunks.size(), before.chunks.size());
    for (std::size_t i = 0; i < after.chunks.size(); ++i) {
        if (i == 4) {
            EXPECT_NE(after.chunks[i], before.chunks[i]);
            EXPECT_TRUE(client_store_.contains(after.chunks[i]));
        } else {
            EXPECT_EQ(after.chunks[i], before.chunks[i]);
            EXPECT_FALSE(client_store_.contains(after.chunks[i]));
        }
    }
    std::copy(patch.begin(), patch.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
    auto got = read_file("remote.bin", 64 * 1024);
    ASSERT_TRUE(got) << got.error().message;
    EXPECT_TRUE(got.value() == data);
}

TEST_F(RemoteRead, ASilentHolderIsPassedOverForTheNextOne) {
    per_request_timeout_ = 300ms;
    start_client();
    // The silent peer gets the smaller key, so it is asked first.
    auto k1 = core::DeviceKey::generate().value();
    auto k2 = core::DeviceKey::generate().value();
    if (k2.public_key() < k1.public_key()) std::swap(k1, k2);
    auto& silent = add_server(Behavior::silent, {}, std::move(k1));
    auto& honest = add_server(Behavior::honest, {}, std::move(k2));
    connect(silent);
    connect(honest);
    auto data = random_bytes(1000, 9);
    publish("f", data, {&honest}, {&silent, &honest});

    auto got = read_file("f", 4096);
    ASSERT_TRUE(got) << got.error().message;
    EXPECT_TRUE(got.value() == data);
}

}  // namespace
}  // namespace ghostdrive::vfs
