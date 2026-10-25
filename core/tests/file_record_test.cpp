#include "ghostdrive/core/file_record.hpp"

#include <gtest/gtest.h>

#include <functional>

#include "ghostdrive/core/bytes.hpp"
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

FileRecord signed_sample() {
    FileRecord r;
    r.path = "a.txt";
    r.size = 5;
    r.mode = 0644;
    r.mtime_ns = -2;
    r.hlc = 0x0102030405060708ull;
    r.version.bump(9);
    r.modified_by = 9;
    r.block_size = kMinBlockSize;
    ChunkId id;
    id.digest.fill(std::byte{0xCC});
    r.chunks = {id};
    return r;
}

DriveId drive_with(std::uint8_t fill) {
    DriveId id{};
    id.fill(std::byte{fill});
    return id;
}

TEST(FileRecord, SignedBytesAreStable) {
    // Changing this layout invalidates every stored signature.
    const std::string expected =
        "67686f737464726976652d66696c652d7265636f72642d7631"  // "ghostdrive-file-record-v1"
        "1111111111111111111111111111111111111111111111111111111111111111"  // drive ID
        "05000000612e747874"                                                // path
        "0500000000000000a4010000feffffffffffffff0807060504030201"          // size .. hlc
        "0100000009000000000000000100000000000000"                          // version
        "0900000000000000000002000000"  // modified_by, block size, chunking, deleted
        "01000000"                      // chunk count
        "01cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";  // tag, digest
    EXPECT_EQ(to_hex(record_signed_bytes(drive_with(0x11), signed_sample())), expected);
}

TEST(FileRecord, SignedBytesCoverEveryReplicatedField) {
    const auto drive = drive_with(0x11);
    const auto key = DeviceKey::generate().value();
    FileRecord base = signed_sample();
    const auto signature = key.sign(record_signed_bytes(drive, base)).value();
    EXPECT_TRUE(verify_signature(key.public_key(), record_signed_bytes(drive, base), signature));
    EXPECT_FALSE(
        verify_signature(key.public_key(), record_signed_bytes(drive_with(0x12), base), signature));

    std::vector<std::pair<const char*, std::function<void(FileRecord&)>>> changes = {
        {"path", [](FileRecord& r) { r.path = "b.txt"; }},
        {"size", [](FileRecord& r) { r.size += 1; }},
        {"mode", [](FileRecord& r) { r.mode = 0600; }},
        {"mtime", [](FileRecord& r) { r.mtime_ns += 1; }},
        {"hlc", [](FileRecord& r) { r.hlc += 1; }},
        {"version", [](FileRecord& r) { r.version.bump(9); }},
        {"modified_by", [](FileRecord& r) { r.modified_by = 10; }},
        {"block_size", [](FileRecord& r) { r.block_size *= 2; }},
        {"deleted", [](FileRecord& r) { r.deleted = true; }},
        {"chunk", [](FileRecord& r) { r.chunks[0].digest[0] = std::byte{0}; }},
        {"chunk count", [](FileRecord& r) { r.chunks.push_back(r.chunks[0]); }},
        {"origin", [](FileRecord& r) { r.origin_path = "other.txt"; }},
    };
    for (const auto& [name, change] : changes) {
        FileRecord changed = base;
        change(changed);
        EXPECT_FALSE(verify_signature(key.public_key(), record_signed_bytes(drive, changed),
                                      signature))
            << name;
    }
    // Local fields are not covered.
    FileRecord local = base;
    local.sequence = 77;
    local.effective_version.bump(3);
    local.signature = signature;
    EXPECT_EQ(record_signed_bytes(drive, local), record_signed_bytes(drive, base));
}

TEST(FileRecord, AConflictCopyKeepsItsOriginsSignature) {
    const auto drive = drive_with(0x11);
    FileRecord original = signed_sample();
    FileRecord copy = original;
    copy.origin_path = original.path;
    copy.path = conflict_copy_path(original.path, original.mtime_ns, original.modified_by);
    EXPECT_TRUE(validate(copy));
    EXPECT_EQ(record_signed_bytes(drive, copy), record_signed_bytes(drive, original));

    // Valid only at the name derived from its origin, mtime and author.
    FileRecord moved = copy;
    moved.path = "elsewhere.txt";
    EXPECT_FALSE(validate(moved));
    FileRecord other_author = copy;
    other_author.modified_by = 0xFFFF000000000000ull;  // the name holds its top 35 bits
    EXPECT_FALSE(validate(other_author));
    FileRecord bad_origin = copy;
    bad_origin.origin_path = "../a.txt";
    bad_origin.path = conflict_copy_path(bad_origin.origin_path, copy.mtime_ns, copy.modified_by);
    EXPECT_FALSE(validate(bad_origin));
}

}  // namespace
}  // namespace ghostdrive::core
