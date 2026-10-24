#include "ghostdrive/core/file_record.hpp"

#include <gtest/gtest.h>

#include "ghostdrive/core/chunking.hpp"

namespace ghostdrive::core {
namespace {

TEST(FileRecord, ValidDrivePaths) {
    EXPECT_TRUE(is_valid_drive_path("a"));
    EXPECT_TRUE(is_valid_drive_path("dir/sub/file.txt"));
    EXPECT_TRUE(is_valid_drive_path(".hidden/..twodots"));

    EXPECT_FALSE(is_valid_drive_path(""));
    EXPECT_FALSE(is_valid_drive_path("/abs"));
    EXPECT_FALSE(is_valid_drive_path("trailing/"));
    EXPECT_FALSE(is_valid_drive_path("a//b"));
    EXPECT_FALSE(is_valid_drive_path("../escape"));
    EXPECT_FALSE(is_valid_drive_path("a/../b"));
    EXPECT_FALSE(is_valid_drive_path("a/./b"));
    EXPECT_FALSE(is_valid_drive_path(std::string_view("a\0b", 3)));
}

TEST(FileRecord, ValidateChunkList) {
    FileRecord r;
    r.path = "f";
    r.size = kMinBlockSize + 1;
    r.block_size = kMinBlockSize;
    r.chunks.resize(2);
    EXPECT_TRUE(validate(r).ok());
    r.chunks.resize(1);
    EXPECT_FALSE(validate(r).ok());

    FileRecord empty;
    empty.path = "e";
    EXPECT_TRUE(validate(empty).ok());

    FileRecord tomb;
    tomb.path = "t";
    tomb.deleted = true;
    EXPECT_TRUE(validate(tomb).ok());
    tomb.chunks.resize(1);
    EXPECT_FALSE(validate(tomb).ok());
}

TEST(FileRecord, ConflictCopyPath) {
    // 2026-10-24 12:34:56 UTC
    const std::int64_t mtime = 1792845296LL * 1'000'000'000LL;
    const std::uint64_t device = 0x0000000000000000ull;
    EXPECT_EQ(conflict_copy_path("dir/movie.mkv", mtime, device),
              "dir/movie.sync-conflict-20261024-123456-AAAAAAA.mkv");
    EXPECT_EQ(conflict_copy_path("README", mtime, device),
              "README.sync-conflict-20261024-123456-AAAAAAA");
    EXPECT_EQ(conflict_copy_path("a/.bashrc", mtime, device),
              "a/.bashrc.sync-conflict-20261024-123456-AAAAAAA");
    EXPECT_EQ(conflict_copy_path("x.tar.gz", mtime, device),
              "x.tar.sync-conflict-20261024-123456-AAAAAAA.gz");
}

TEST(FileRecord, ConflictCopyPathEncodesDevice) {
    const std::uint64_t device = 0xFFFFFFFFFFFFFFFFull;
    const std::string p = conflict_copy_path("f.txt", 0, device);
    EXPECT_EQ(p, "f.sync-conflict-19700101-000000-7777777.txt");
    EXPECT_TRUE(is_valid_drive_path(p));
}


TEST(FileRecord, BlockSizeMustBeAPowerOfTwoInRange) {
    FileRecord r;
    r.path = "f";
    r.size = 1;
    r.chunks.resize(1);
    for (std::uint32_t ok : {kMinBlockSize, 1024u * 1024, kMaxBlockSize}) {
        r.block_size = ok;
        EXPECT_TRUE(validate(r)) << ok;
    }
    for (std::uint32_t bad : {0u, 4096u, kMinBlockSize + 1, 3u * kMinBlockSize, 2 * kMaxBlockSize,
                              0xFFFFFFFFu}) {
        r.block_size = bad;
        EXPECT_FALSE(validate(r)) << bad;
    }
}

}  // namespace
}  // namespace ghostdrive::core
