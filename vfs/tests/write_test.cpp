#include <gtest/gtest.h>

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

class WriteTest : public ::testing::Test {
protected:
    WriteTest()
        : secret_(core::generate_drive_secret().value()),
          store_(core::ChunkStore::open(dir_.path() / "store", core::ChunkHasher(secret_)).value()),
          index_(core::MetadataIndex::open(dir_.path() / "index.db", 0xA1).value()),
          cache_(64 * 1024 * 1024),
          fetcher_(io_.get_executor(), disk_.get_executor(), store_, index_, cache_, nullptr),
          fs_(io_.get_executor(), tree_, fetcher_, {},
              WriteSupport{store_, index_, clock_, disk_.get_executor(), dir_.path() / "staging"}) {
    }

    ~WriteTest() override {
        drain([] { return false; }, 50ms);  // let background commits finish
        disk_.join();
    }

    template <typename Done>
    void drain(Done done, std::chrono::milliseconds limit = 10s) {
        auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < deadline) io_.run_one_for(5ms);
    }

    core::Status wait_status(std::function<void(Filesystem::StatusCallback)> start) {
        std::optional<core::Status> result;
        start([&](core::Status st) { result = std::move(st); });
        drain([&] { return result.has_value(); });
        return result ? *result : core::Status(core::Error{core::Errc::timeout, "drain"});
    }

    core::Status write(std::uint64_t h, std::uint64_t offset, const core::Bytes& data) {
        return wait_status([&](auto cb) { fs_.write(h, offset, data, std::move(cb)); });
    }
    core::Status flush(std::uint64_t h) {
        return wait_status([&](auto cb) { fs_.flush(h, std::move(cb)); });
    }
    core::Result<Attr> setattr(Ino ino, SetAttr changes, std::optional<std::uint64_t> h = {}) {
        std::optional<core::Result<Attr>> result;
        fs_.setattr(ino, changes, h, [&](core::Result<Attr> r) { result = std::move(r); });
        drain([&] { return result.has_value(); });
        if (!result) return core::Error{core::Errc::timeout, "drain"};
        return std::move(*result);
    }

    core::Bytes read_all(Ino ino) {
        auto h = fs_.open(ino);
        EXPECT_TRUE(h);
        auto size = fs_.getattr(ino)->size;
        std::optional<core::Result<ReadReply>> result;
        fs_.read(h.value(), 0, static_cast<std::size_t>(size),
                 [&](core::Result<ReadReply> r) { result = std::move(r); });
        drain([&] { return result.has_value(); });
        fs_.release(h.value());
        core::Bytes out;
        if (!result || !*result) {
            ADD_FAILURE() << "read failed";
            return out;
        }
        for (auto s : result->value().slices) out.insert(out.end(), s.begin(), s.end());
        return out;
    }

    // Creates `path` (in the root) with `data` and returns its inode.
    Ino make_file(const std::string& name, const core::Bytes& data) {
        auto created = fs_.create(kRootIno, name, 0644);
        EXPECT_TRUE(created) << created.error().message;
        auto h = created.value().second;
        EXPECT_TRUE(write(h, 0, data));
        EXPECT_TRUE(flush(h));
        fs_.release(h);
        return created.value().first.ino;
    }

    core::FileRecord record_of(const std::string& path) {
        auto r = index_.get(path);
        EXPECT_TRUE(r && r.value());
        return *r.value();
    }

    test::TempDir dir_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_{io_.get_executor()};
    asio::thread_pool disk_{2};
    core::DriveSecret secret_;
    core::ChunkStore store_;
    core::MetadataIndex index_;
    core::HybridClock clock_;
    core::ChunkCache cache_;
    Tree tree_;
    Fetcher fetcher_;
    Filesystem fs_;
};

TEST_F(WriteTest, CreateWriteFlushCommitsANewVersion) {
    auto data = random_bytes(3 * kBlock + 77, 1);
    auto created = fs_.create(kRootIno, "new.bin", 0640);
    ASSERT_TRUE(created) << created.error().message;
    auto ino = created.value().first.ino;
    auto h = created.value().second;
    EXPECT_EQ(record_of("new.bin").size, 0u);  // visible right away, empty

    ASSERT_TRUE(write(h, 0, data));
    EXPECT_EQ(fs_.getattr(ino)->size, data.size());  // size of the file being written
    ASSERT_TRUE(flush(h));
    fs_.release(h);

    auto record = record_of("new.bin");
    EXPECT_EQ(record.size, data.size());
    EXPECT_EQ(record.block_size, core::block_size_for(data.size()));
    EXPECT_EQ(record.chunks.size(), 4u);
    EXPECT_EQ(record.mode & 07777, 0640u);
    for (const auto& id : record.chunks) EXPECT_TRUE(store_.contains(id));
    EXPECT_EQ(tree_.resolve("new.bin"), ino);
    EXPECT_TRUE(read_all(ino) == data);
}

TEST_F(WriteTest, PartialOverwriteRehashesOnlyTheTouchedBlock) {
    auto data = random_bytes(5 * kBlock, 2);
    auto ino = make_file("f", data);
    auto before = record_of("f");

    auto h = fs_.open(ino, true);
    ASSERT_TRUE(h);
    auto patch = random_bytes(100, 3);
    std::uint64_t offset = 2 * kBlock + 1000;
    ASSERT_TRUE(write(h.value(), offset, patch));
    ASSERT_TRUE(flush(h.value()));
    fs_.release(h.value());

    auto after = record_of("f");
    ASSERT_EQ(after.chunks.size(), before.chunks.size());
    for (std::size_t i = 0; i < after.chunks.size(); ++i) {
        if (i == 2) {
            EXPECT_NE(after.chunks[i], before.chunks[i]);
        } else {
            EXPECT_EQ(after.chunks[i], before.chunks[i]) << "block " << i << " was rewritten";
        }
    }
    std::copy(patch.begin(), patch.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
    EXPECT_TRUE(read_all(ino) == data);
    EXPECT_EQ(after.version.compare(before.version), core::VersionVector::Order::after);
}

TEST_F(WriteTest, WriteHandleReadsItsOwnWrites) {
    auto data = random_bytes(2 * kBlock, 4);
    auto ino = make_file("f", data);
    auto h = fs_.open(ino, true);
    ASSERT_TRUE(h);
    auto patch = random_bytes(10, 5);
    ASSERT_TRUE(write(h.value(), kBlock - 5, patch));  // straddles a block boundary

    std::optional<core::Result<ReadReply>> result;
    fs_.read(h.value(), kBlock - 10, 20, [&](core::Result<ReadReply> r) { result = std::move(r); });
    drain([&] { return result.has_value(); });
    ASSERT_TRUE(result && *result);
    core::Bytes got;
    for (auto s : result->value().slices) got.insert(got.end(), s.begin(), s.end());
    std::copy(patch.begin(), patch.end(), data.begin() + (kBlock - 5));
    EXPECT_TRUE(got == core::Bytes(data.begin() + (kBlock - 10), data.begin() + (kBlock + 10)));
    fs_.release(h.value());
}

TEST_F(WriteTest, TruncateShrinksAndGrowsWithZeros) {
    auto data = random_bytes(3 * kBlock + 500, 6);
    auto ino = make_file("f", data);
    auto before = record_of("f");

    auto shrunk = setattr(ino, SetAttr{.size = kBlock + 10});
    ASSERT_TRUE(shrunk) << shrunk.error().message;
    EXPECT_EQ(shrunk.value().size, kBlock + 10);
    auto mid = record_of("f");
    ASSERT_EQ(mid.chunks.size(), 2u);
    EXPECT_EQ(mid.chunks[0], before.chunks[0]);  // untouched block keeps its ID
    EXPECT_TRUE(read_all(ino) == core::Bytes(data.begin(), data.begin() + kBlock + 10));

    auto grown = setattr(ino, SetAttr{.size = 2 * kBlock + 20});
    ASSERT_TRUE(grown);
    auto expected = core::Bytes(data.begin(), data.begin() + kBlock + 10);
    expected.resize(2 * kBlock + 20, std::byte{0});
    EXPECT_TRUE(read_all(ino) == expected);
}

TEST_F(WriteTest, WritePastTheEndLeavesAZeroHole) {
    auto ino = make_file("f", random_bytes(100, 7));
    auto original = read_all(ino);
    auto h = fs_.open(ino, true);
    ASSERT_TRUE(h);
    auto tail = random_bytes(50, 8);
    ASSERT_TRUE(write(h.value(), 2 * kBlock, tail));
    ASSERT_TRUE(flush(h.value()));
    fs_.release(h.value());

    auto expected = original;
    expected.resize(2 * kBlock, std::byte{0});
    expected.insert(expected.end(), tail.begin(), tail.end());
    EXPECT_TRUE(read_all(ino) == expected);
}

TEST_F(WriteTest, TruncatingOpenRechoosesTheBlockSize) {
    auto ino = make_file("f", random_bytes(1000, 9));
    EXPECT_EQ(record_of("f").block_size, core::kMinBlockSize);
    auto h = fs_.open(ino, true, true);
    ASSERT_TRUE(h);
    EXPECT_EQ(fs_.getattr(ino)->size, 0u);
    // 300 MiB would need more than 2000 blocks of 128 KiB; write sparsely to keep it cheap.
    std::uint64_t big = 300ull * 1024 * 1024;
    ASSERT_TRUE(write(h.value(), big - 10, random_bytes(10, 10)));
    ASSERT_TRUE(flush(h.value()));
    fs_.release(h.value());
    auto record = record_of("f");
    EXPECT_EQ(record.size, big);
    EXPECT_EQ(record.block_size, core::block_size_for(big));
    EXPECT_GT(record.block_size, core::kMinBlockSize);
}

TEST_F(WriteTest, DirectoriesCanBeEmptyAndRemoved) {
    auto made = fs_.mkdir(kRootIno, "docs", 0750);
    ASSERT_TRUE(made) << made.error().message;
    EXPECT_EQ(made.value().type, NodeType::directory);
    EXPECT_EQ(made.value().perm, 0750u);
    EXPECT_TRUE(tree_.readdir(made.value().ino)->empty());
    EXPECT_FALSE(fs_.mkdir(kRootIno, "docs", 0755));  // exists

    auto created = fs_.create(made.value().ino, "a.txt", 0644);
    ASSERT_TRUE(created);
    fs_.release(created.value().second);
    auto rm = fs_.rmdir(kRootIno, "docs");
    ASSERT_FALSE(rm);
    EXPECT_EQ(rm.error().code, core::Errc::not_empty);

    ASSERT_TRUE(fs_.unlink(made.value().ino, "a.txt"));
    EXPECT_TRUE(tree_.resolve("docs"));  // explicit directory survives its last file
    ASSERT_TRUE(fs_.rmdir(kRootIno, "docs"));
    EXPECT_FALSE(tree_.resolve("docs"));
}

TEST_F(WriteTest, UnlinkWhileOpenDiscardsTheWrites) {
    auto ino = make_file("f", random_bytes(10, 11));
    auto h = fs_.open(ino, true);
    ASSERT_TRUE(h);
    ASSERT_TRUE(fs_.unlink(kRootIno, "f"));
    ASSERT_TRUE(write(h.value(), 0, random_bytes(10, 12)));  // the descriptor still works
    ASSERT_TRUE(flush(h.value()));
    fs_.release(h.value());
    EXPECT_FALSE(tree_.resolve("f"));
    auto r = index_.get("f");
    ASSERT_TRUE(r);
    EXPECT_TRUE(r.value()->deleted);
}

TEST_F(WriteTest, RenameKeepsInodesAndMovesSubtrees) {
    auto file_ino = make_file("a.txt", random_bytes(10, 13));
    auto dir = fs_.mkdir(kRootIno, "src", 0755);
    ASSERT_TRUE(dir);
    auto inner = fs_.create(dir.value().ino, "inner.bin", 0644);
    ASSERT_TRUE(inner);
    auto inner_ino = inner.value().first.ino;
    fs_.release(inner.value().second);

    ASSERT_TRUE(fs_.rename(kRootIno, "a.txt", dir.value().ino, "b.txt"));
    EXPECT_EQ(tree_.resolve("src/b.txt"), file_ino);
    EXPECT_FALSE(tree_.resolve("a.txt"));

    ASSERT_TRUE(fs_.rename(kRootIno, "src", kRootIno, "dst"));
    EXPECT_EQ(tree_.resolve("dst"), dir.value().ino);
    EXPECT_EQ(tree_.resolve("dst/inner.bin"), inner_ino);
    EXPECT_EQ(tree_.resolve("dst/b.txt"), file_ino);
    EXPECT_TRUE(index_.get("src/inner.bin").value()->deleted);
    EXPECT_FALSE(index_.get("dst/inner.bin").value()->deleted);

    auto into_self = fs_.rename(kRootIno, "dst", dir.value().ino, "loop");
    ASSERT_FALSE(into_self);
    EXPECT_EQ(into_self.error().code, core::Errc::invalid_argument);
}

TEST_F(WriteTest, RenameOverAFileReplacesIt) {
    auto keep = random_bytes(500, 14);
    auto a = make_file("a", keep);
    make_file("b", random_bytes(800, 15));
    EXPECT_FALSE(fs_.rename(kRootIno, "a", kRootIno, "b", true));  // no_replace
    ASSERT_TRUE(fs_.rename(kRootIno, "a", kRootIno, "b"));
    EXPECT_EQ(tree_.resolve("b"), a);
    EXPECT_TRUE(read_all(a) == keep);
}

TEST_F(WriteTest, RenameFollowsAnOpenWriter) {
    auto ino = make_file("draft", random_bytes(10, 16));
    auto h = fs_.open(ino, true);
    ASSERT_TRUE(h);
    ASSERT_TRUE(fs_.rename(kRootIno, "draft", kRootIno, "final"));
    auto data = random_bytes(20, 17);
    ASSERT_TRUE(write(h.value(), 0, data));
    ASSERT_TRUE(flush(h.value()));
    fs_.release(h.value());
    EXPECT_EQ(record_of("final").size, 20u);
    EXPECT_TRUE(index_.get("draft").value()->deleted);  // not resurrected by the commit
}

TEST_F(WriteTest, ChmodAndTouchWithoutAWriter) {
    auto ino = make_file("f", random_bytes(10, 18));
    auto attr = setattr(ino, SetAttr{.perm = 0600, .mtime_ns = 1'234'000'000'000});
    ASSERT_TRUE(attr) << attr.error().message;
    EXPECT_EQ(attr.value().perm, 0600u);
    auto record = record_of("f");
    EXPECT_EQ(record.mode & 07777, 0600u);
    EXPECT_EQ(record.mtime_ns, 1'234'000'000'000);
}

TEST_F(WriteTest, ReadOnlyFilesystemRejectsChanges) {
    Tree tree;
    Filesystem ro(io_.get_executor(), tree, fetcher_);
    EXPECT_FALSE(ro.writable());
    auto created = ro.create(kRootIno, "x", 0644);
    ASSERT_FALSE(created);
    EXPECT_EQ(to_errno(created.error()), EROFS);
    EXPECT_EQ(ro.mkdir(kRootIno, "d", 0755).error().code, core::Errc::read_only);
}

TEST_F(WriteTest, InvalidNamesAreRejected) {
    EXPECT_FALSE(fs_.create(kRootIno, "..", 0644));
    EXPECT_FALSE(fs_.create(kRootIno, "a/b", 0644));
    EXPECT_FALSE(fs_.create(kRootIno, "", 0644));
    auto file = make_file("plain", random_bytes(1, 19));
    auto under_file = fs_.create(file, "child", 0644);
    ASSERT_FALSE(under_file);
    EXPECT_EQ(under_file.error().code, core::Errc::not_directory);
}

}  // namespace
}  // namespace ghostdrive::vfs
