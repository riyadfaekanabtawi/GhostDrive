#include "ghostdrive/vfs/file_view.hpp"

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>
#include <fstream>
#include <random>

#include "ghostdrive/core/chunking.hpp"
#include "ghostdrive/vfs/filesystem.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::vfs {
namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kBlock = core::kMinBlockSize;

core::Bytes random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    core::Bytes data(size);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    return data;
}

core::Bytes concat(const ReadReply& reply) {
    core::Bytes out;
    for (auto s : reply.slices) out.insert(out.end(), s.begin(), s.end());
    return out;
}

class FileViewTest : public ::testing::Test {
protected:
    FileViewTest()
        : secret_(core::generate_drive_secret().value()),
          store_(core::ChunkStore::open(dir_.path() / "store", core::ChunkHasher(secret_)).value()),
          index_(core::MetadataIndex::open(dir_.path() / "index.db", 0xA1).value()),
          cache_(64 * 1024 * 1024),
          fetcher_(io_.get_executor(), disk_.get_executor(), store_, index_, cache_, nullptr) {}

    ~FileViewTest() override { disk_.join(); }

    // Stores `data` as a file and returns its record.
    core::FileRecord make_file(const std::string& path, const core::Bytes& data) {
        auto source = dir_.path() / ("src-" + std::to_string(counter_++));
        std::ofstream(source, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        auto imported = store_.import_file(source);
        EXPECT_TRUE(imported) << imported.error().message;
        core::FileRecord record;
        record.path = path;
        record.size = imported.value().size;
        record.mode = 0644;
        record.block_size = imported.value().block_size;
        record.chunks = imported.value().chunks;
        return record;
    }

    core::Result<ReadReply> read(FileView& view, std::uint64_t offset, std::size_t size) {
        std::optional<core::Result<ReadReply>> result;
        asio::co_spawn(
            io_, [&]() -> asio::awaitable<void> { result = co_await view.read(offset, size); },
            asio::detached);
        drain([&] { return result.has_value(); });
        if (!result) return core::Error{core::Errc::timeout, "test drain timed out"};
        return std::move(*result);
    }

    template <typename Done>
    void drain(Done done) {
        auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!done() && std::chrono::steady_clock::now() < deadline) io_.run_one_for(10ms);
        ASSERT_TRUE(done()) << "timed out";
    }

    test::TempDir dir_;
    asio::io_context io_;
    // Keeps run_one_for from stopping the context whenever it briefly runs out of work.
    asio::executor_work_guard<asio::io_context::executor_type> work_{io_.get_executor()};
    asio::thread_pool disk_{2};
    core::DriveSecret secret_;
    core::ChunkStore store_;
    core::MetadataIndex index_;
    core::ChunkCache cache_;
    Fetcher fetcher_;
    int counter_ = 0;
};

TEST_F(FileViewTest, ReadWithinOneChunk) {
    auto data = random_bytes(kBlock * 2 + 1000, 1);
    FileView view(make_file("f", data), fetcher_, 0);
    auto reply = read(view, 100, 4096);
    ASSERT_TRUE(reply) << reply.error().message;
    ASSERT_EQ(reply.value().slices.size(), 1u);
    EXPECT_EQ(concat(reply.value()), core::Bytes(data.begin() + 100, data.begin() + 100 + 4096));
}

TEST_F(FileViewTest, ReadAcrossABoundaryUsesTwoSlicesOfTheChunkBuffers) {
    auto data = random_bytes(kBlock * 2 + 1000, 2);
    FileView view(make_file("f", data), fetcher_, 0);
    std::uint64_t offset = kBlock - 10;
    auto reply = read(view, offset, 20);
    ASSERT_TRUE(reply) << reply.error().message;
    ASSERT_EQ(reply.value().slices.size(), 2u);
    EXPECT_EQ(reply.value().slices[0].size(), 10u);
    EXPECT_EQ(reply.value().slices[1].size(), 10u);
    // The slices point into the cached chunk buffers: no copy was made.
    EXPECT_EQ(reply.value().slices[0].data(), reply.value().chunks[0]->data() + (kBlock - 10));
    EXPECT_EQ(reply.value().slices[1].data(), reply.value().chunks[1]->data());
    EXPECT_EQ(concat(reply.value()),
              core::Bytes(data.begin() + static_cast<std::ptrdiff_t>(offset),
                          data.begin() + static_cast<std::ptrdiff_t>(offset) + 20));
}

TEST_F(FileViewTest, ReadsClipAtTheEndOfTheFile) {
    auto data = random_bytes(kBlock + 500, 3);
    FileView view(make_file("f", data), fetcher_, 0);
    auto tail = read(view, kBlock + 400, 4096);
    ASSERT_TRUE(tail);
    EXPECT_EQ(tail.value().size(), 100u);
    auto past = read(view, kBlock + 500, 4096);
    ASSERT_TRUE(past);
    EXPECT_EQ(past.value().size(), 0u);
    auto empty = read(view, 0, 0);
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty.value().size(), 0u);
}

TEST_F(FileViewTest, SequentialReadsGrowTheReadaheadWindow) {
    auto data = random_bytes(kBlock * 12, 4);
    FileView view(make_file("f", data), fetcher_, 4);
    std::uint64_t offset = 0;
    for (int i = 0; i < 4; ++i) {
        auto reply = read(view, offset, 32 * 1024);
        ASSERT_TRUE(reply);
        offset += 32 * 1024;
    }
    EXPECT_EQ(view.readahead_window(), 4u);
    drain([&] {
        for (std::size_t i = 1; i <= 4; ++i) {
            if (!cache_.contains(view.record().chunks[i])) return false;
        }
        return true;
    });
    EXPECT_FALSE(cache_.contains(view.record().chunks[6]));

    // A random jump resets the window.
    auto reply = read(view, kBlock * 11, 4096);
    ASSERT_TRUE(reply);
    EXPECT_EQ(view.readahead_window(), 0u);
}

TEST_F(FileViewTest, MissingChunkWithNoPeersIsNotFound) {
    auto record = make_file("f", random_bytes(kBlock, 5));
    ASSERT_TRUE(store_.remove(record.chunks[0]));
    FileView view(record, fetcher_, 0);
    auto reply = read(view, 0, 10);
    ASSERT_FALSE(reply);
    EXPECT_EQ(reply.error().code, core::Errc::not_found);
}

TEST_F(FileViewTest, ConcurrentReadsOfOneChunkShareOneLoad) {
    auto data = random_bytes(kBlock, 6);
    FileView a(make_file("f", data), fetcher_, 0);
    FileView b(a.record(), fetcher_, 0);
    std::optional<core::Result<ReadReply>> ra;
    std::optional<core::Result<ReadReply>> rb;
    asio::co_spawn(
        io_, [&]() -> asio::awaitable<void> { ra = co_await a.read(0, 10); }, asio::detached);
    asio::co_spawn(
        io_, [&]() -> asio::awaitable<void> { rb = co_await b.read(5, 10); }, asio::detached);
    drain([&] { return ra && rb; });
    ASSERT_TRUE(*ra);
    ASSERT_TRUE(*rb);
    EXPECT_EQ(fetcher_.stats().local_reads, 1u);
    EXPECT_EQ(fetcher_.stats().coalesced, 1u);
}

TEST_F(FileViewTest, OpenPinsTheVersion) {
    auto v1 = random_bytes(5000, 7);
    auto v2 = random_bytes(6000, 8);
    Tree tree;
    tree.apply(make_file("doc", v1));
    Filesystem fs(io_.get_executor(), tree, fetcher_);

    auto ino = tree.resolve("doc");
    ASSERT_TRUE(ino);
    auto handle = fs.open(*ino);
    ASSERT_TRUE(handle);
    tree.apply(make_file("doc", v2));  // a remote edit arrives while the file is open

    auto read_handle = [&](std::uint64_t h) {
        std::optional<core::Result<ReadReply>> result;
        fs.read(h, 0, 10000, [&](core::Result<ReadReply> r) { result = std::move(r); });
        drain([&] { return result.has_value(); });
        return concat(result->value());
    };
    EXPECT_EQ(read_handle(handle.value()), v1);

    auto reopened = fs.open(*ino);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(read_handle(reopened.value()), v2);

    fs.release(handle.value());
    fs.release(reopened.value());
    EXPECT_EQ(fs.open_handles(), 0u);
    EXPECT_FALSE(fs.open(kRootIno));  // directories cannot be opened as files
}

}  // namespace
}  // namespace ghostdrive::vfs
