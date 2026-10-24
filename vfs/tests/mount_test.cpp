// Real mounts through libfuse3 (FUSE-T on macOS). The filesystem's executor runs on a
// background thread while the test thread uses ordinary POSIX calls on the mountpoint.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <set>
#include <thread>

#include "ghostdrive/core/chunking.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "ghostdrive/vfs/chunk_exchange.hpp"
#include "ghostdrive/vfs/fuse_mount.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::vfs {
namespace {

using namespace std::chrono_literals;

core::Bytes random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    core::Bytes data(size);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    return data;
}

core::Bytes read_whole(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    core::Bytes out(chars.size());
    std::memcpy(out.data(), chars.data(), chars.size());
    return out;
}

class Mount : public ::testing::Test {
protected:
    Mount()
        : secret_(core::generate_drive_secret().value()),
          store_(core::ChunkStore::open(dir_.path() / "store", core::ChunkHasher(secret_)).value()),
          index_(core::MetadataIndex::open(dir_.path() / "index.db", 0xA1).value()),
          cache_(128 * 1024 * 1024) {
        mountpoint_ = dir_.path() / "mnt";
        std::filesystem::create_directories(mountpoint_);
    }

    ~Mount() override {
        mount_.reset();  // unmount before the executor and the filesystem go away
        if (io_thread_.joinable()) {
            std::promise<void> closed;
            asio::post(io_, [&] {
                if (client_) client_->close();
                if (server_) server_->close();
                closed.set_value();
            });
            closed.get_future().wait();
        }
        work_.reset();
        io_.stop();
        if (io_thread_.joinable()) io_thread_.join();
        disk_.join();
    }

    void remount() {
        mount_.reset();
        auto mounted = FuseMount::mount(*fs_, mountpoint_);
        ASSERT_TRUE(mounted) << mounted.error().message;
        mount_ = std::move(mounted).value();
    }

    std::optional<core::FileRecord> indexed(const std::string& path) {
        auto r = index_.get(path);
        if (!r || !r.value() || r.value()->deleted) return std::nullopt;
        return *r.value();
    }

    void start(ConnectionLookup peers = nullptr, bool writable = false) {
        fetcher_ = std::make_unique<Fetcher>(io_.get_executor(), disk_.get_executor(), store_,
                                             index_, cache_, std::move(peers));
        std::optional<WriteSupport> writes;
        if (writable) {
            writes.emplace(WriteSupport{store_, index_, clock_, disk_.get_executor(),
                                        dir_.path() / "staging"});
        }
        fs_ = std::make_unique<Filesystem>(io_.get_executor(), tree_, *fetcher_,
                                           FilesystemOptions{}, std::move(writes));
        io_thread_ = std::thread([this] { io_.run(); });
        auto mounted = FuseMount::mount(*fs_, mountpoint_);
        ASSERT_TRUE(mounted) << mounted.error().message;
        mount_ = std::move(mounted).value();
    }

    core::FileRecord add(core::ChunkStore& store, const std::string& path,
                         const core::Bytes& data) {
        auto source = dir_.path() / ("src-" + std::to_string(counter_++));
        std::ofstream(source, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        auto imported = store.import_file(source).value();
        core::FileRecord record;
        record.path = path;
        record.size = imported.size;
        record.mode = 0644;
        record.mtime_ns = 1'700'000'000'000'000'000;
        record.block_size = imported.block_size;
        record.chunks = imported.chunks;
        EXPECT_TRUE(index_.commit_local(record, clock_));
        EXPECT_TRUE(tree_.refresh(index_));
        return record;
    }

    test::TempDir dir_;
    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_{
        io_.get_executor()};
    asio::thread_pool disk_{2};
    std::thread io_thread_;
    core::DriveSecret secret_;
    core::ChunkStore store_;
    core::MetadataIndex index_;
    core::HybridClock clock_;
    core::ChunkCache cache_;
    Tree tree_;
    std::unique_ptr<Fetcher> fetcher_;
    std::unique_ptr<Filesystem> fs_;
    std::filesystem::path mountpoint_;
    std::unique_ptr<FuseMount> mount_;
    std::shared_ptr<transport::Endpoint> client_;
    std::shared_ptr<transport::Endpoint> server_;
    int counter_ = 0;
};

TEST_F(Mount, ListsStatsAndReadsFiles) {
    auto small = random_bytes(5000, 1);
    auto large = random_bytes(3 * core::kMinBlockSize + 12345, 2);
    add(store_, "notes.txt", small);
    add(store_, "media/clip.bin", large);
    start();

    std::set<std::string> top;
    for (const auto& e : std::filesystem::directory_iterator(mountpoint_)) {
        top.insert(e.path().filename().string());
    }
    EXPECT_EQ(top, (std::set<std::string>{"media", "notes.txt"}));
    EXPECT_TRUE(std::filesystem::is_directory(mountpoint_ / "media"));

    struct stat st{};
    ASSERT_EQ(::stat((mountpoint_ / "media/clip.bin").c_str(), &st), 0) << std::strerror(errno);
    EXPECT_EQ(static_cast<std::uint64_t>(st.st_size), large.size());
    EXPECT_TRUE(S_ISREG(st.st_mode));

    EXPECT_TRUE(read_whole(mountpoint_ / "notes.txt") == small);
    EXPECT_TRUE(read_whole(mountpoint_ / "media/clip.bin") == large);
}

TEST_F(Mount, RandomAccessReadsReturnTheRightBytes) {
    auto data = random_bytes(6 * core::kMinBlockSize, 3);
    add(store_, "f.bin", data);
    start();

    int fd = ::open((mountpoint_ / "f.bin").c_str(), O_RDONLY);
    ASSERT_GE(fd, 0) << std::strerror(errno);
    std::mt19937_64 rng(4);
    for (int i = 0; i < 50; ++i) {
        std::size_t len = 1 + rng() % 70000;
        std::size_t off = rng() % (data.size() - len);
        core::Bytes buf(len);
        ssize_t n = ::pread(fd, buf.data(), len, static_cast<off_t>(off));
        ASSERT_EQ(n, static_cast<ssize_t>(len)) << "offset " << off;
        ASSERT_TRUE(
            std::equal(buf.begin(), buf.end(), data.begin() + static_cast<std::ptrdiff_t>(off)))
            << "mismatch at offset " << off;
    }
    ::close(fd);
}

TEST_F(Mount, IsReadOnlyAndReportsMissingFiles) {
    add(store_, "f", random_bytes(10, 5));
    start();
    errno = 0;
    EXPECT_LT(::open((mountpoint_ / "f").c_str(), O_WRONLY), 0);
    EXPECT_TRUE(errno == EROFS || errno == EACCES) << std::strerror(errno);
    errno = 0;
    EXPECT_LT(::open((mountpoint_ / "missing").c_str(), O_RDONLY), 0);
    EXPECT_EQ(errno, ENOENT);
    errno = 0;
    EXPECT_LT(::open((mountpoint_ / "new").c_str(), O_CREAT | O_WRONLY, 0644), 0);
}

TEST_F(Mount, ReadsAFileWhoseChunksAreOnlyOnAPeer) {
    // A second store and endpoint act as the device that holds the file.
    auto server_store =
        core::ChunkStore::open(dir_.path() / "server-store", core::ChunkHasher(secret_)).value();
    auto server_key = core::DeviceKey::generate().value();
    auto client_key = core::DeviceKey::generate().value();
    transport::EndpointOptions server_options;
    server_options.accept_peer = [pin = client_key.public_key()](const core::PublicKey& k) {
        return k == pin;
    };
    server_ = transport::Endpoint::create(io_.get_executor(), server_key, server_options).value();
    client_ = transport::Endpoint::create(io_.get_executor(), client_key, {}).value();

    auto data = random_bytes(4 * core::kMinBlockSize + 99, 6);
    auto record = add(server_store, "remote/video.bin", data);
    ASSERT_TRUE(index_.add_device(server_key.public_key(), "server"));
    for (const auto& id : record.chunks) {
        ASSERT_TRUE(index_.set_holds(server_key.public_key(), id, true));
        ASSERT_FALSE(store_.contains(id));
    }

    std::shared_ptr<transport::Connection> conn;
    start([&conn](const core::PublicKey&) { return conn; });

    std::promise<core::Result<std::shared_ptr<transport::Connection>>> connected;
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            asio::co_spawn(
                io_,
                [&]() -> asio::awaitable<void> {
                    auto incoming = co_await server_->accept();
                    if (incoming) {
                        co_await serve_chunks(incoming.value(), server_store, disk_.get_executor());
                    }
                },
                asio::detached);
            connected.set_value(
                co_await client_->connect(server_->local_endpoint(), server_key.public_key()));
        },
        asio::detached);
    auto result = connected.get_future().get();
    ASSERT_TRUE(result) << result.error().message;
    std::promise<void> stored;
    asio::post(io_, [&] {
        conn = result.value();
        stored.set_value();
    });
    stored.get_future().wait();

    EXPECT_TRUE(read_whole(mountpoint_ / "remote/video.bin") == data);
    std::promise<FetcherStats> stats;
    asio::post(io_, [&] { stats.set_value(fetcher_->stats()); });
    auto s = stats.get_future().get();
    EXPECT_EQ(s.remote_fetches, record.chunks.size());
    EXPECT_EQ(s.hash_mismatches, 0u);
}

TEST_F(Mount, WritesThroughTheMountReachTheIndex) {
    start(nullptr, true);
    auto file = mountpoint_ / "hello.txt";
    { std::ofstream(file) << "hello"; }
    { std::ofstream(file, std::ios::app) << ", world\n"; }
    EXPECT_EQ(read_whole(file).size(), 13u);
    auto record = indexed("hello.txt");
    ASSERT_TRUE(record);
    EXPECT_EQ(record->size, 13u);

    // A multi-block file, then an in-place patch in the middle.
    auto big = random_bytes(3 * core::kMinBlockSize + 4321, 7);
    {
        std::ofstream out(mountpoint_ / "big.bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(big.data()), static_cast<std::streamsize>(big.size()));
    }
    auto before = indexed("big.bin");
    ASSERT_TRUE(before);
    EXPECT_EQ(before->size, big.size());
    int fd = ::open((mountpoint_ / "big.bin").c_str(), O_RDWR);
    ASSERT_GE(fd, 0) << std::strerror(errno);
    auto patch = random_bytes(64, 8);
    off_t offset = static_cast<off_t>(core::kMinBlockSize) + 100;
    ASSERT_EQ(::pwrite(fd, patch.data(), patch.size(), offset), static_cast<ssize_t>(patch.size()));
    ASSERT_EQ(::close(fd), 0) << std::strerror(errno);
    std::copy(patch.begin(), patch.end(), big.begin() + offset);
    EXPECT_TRUE(read_whole(mountpoint_ / "big.bin") == big);
    auto after = indexed("big.bin");
    ASSERT_TRUE(after);
    ASSERT_EQ(after->chunks.size(), before->chunks.size());
    EXPECT_EQ(after->chunks[0], before->chunks[0]);
    EXPECT_NE(after->chunks[1], before->chunks[1]);
    EXPECT_EQ(after->chunks[2], before->chunks[2]);

    ASSERT_EQ(::truncate((mountpoint_ / "big.bin").c_str(), 1000), 0) << std::strerror(errno);
    EXPECT_EQ(std::filesystem::file_size(mountpoint_ / "big.bin"), 1000u);
    EXPECT_EQ(indexed("big.bin")->size, 1000u);
}

TEST_F(Mount, DirectoryOperationsThroughTheMount) {
    start(nullptr, true);
    std::filesystem::create_directory(mountpoint_ / "docs");
    { std::ofstream(mountpoint_ / "docs" / "a.txt") << "a"; }
    std::filesystem::create_directories(mountpoint_ / "empty" / "nested");
    EXPECT_TRUE(std::filesystem::is_directory(mountpoint_ / "empty" / "nested"));

    std::filesystem::rename(mountpoint_ / "docs" / "a.txt", mountpoint_ / "docs" / "b.txt");
    EXPECT_FALSE(std::filesystem::exists(mountpoint_ / "docs" / "a.txt"));
    EXPECT_EQ(read_whole(mountpoint_ / "docs" / "b.txt").size(), 1u);

    std::filesystem::rename(mountpoint_ / "docs", mountpoint_ / "papers");
    EXPECT_TRUE(std::filesystem::exists(mountpoint_ / "papers" / "b.txt"));
    EXPECT_TRUE(indexed("papers/b.txt"));
    EXPECT_FALSE(indexed("docs/b.txt"));

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::remove(mountpoint_ / "papers", ec));  // not empty
    EXPECT_TRUE(std::filesystem::remove(mountpoint_ / "papers" / "b.txt"));
    EXPECT_TRUE(std::filesystem::remove(mountpoint_ / "papers"));
    EXPECT_FALSE(std::filesystem::exists(mountpoint_ / "papers"));
    EXPECT_FALSE(indexed("papers/b.txt"));
}

TEST_F(Mount, WrittenFilesSurviveARemount) {
    start(nullptr, true);
    auto data = random_bytes(200'000, 9);
    {
        std::ofstream out(mountpoint_ / "keep.bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    remount();
    EXPECT_TRUE(read_whole(mountpoint_ / "keep.bin") == data);
}

}  // namespace
}  // namespace ghostdrive::vfs
