#include "ghostdrive/core/chunk_store.hpp"

#include <gtest/gtest.h>

#include <fstream>

#include "ghostdrive/core/chunking.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::core {
namespace {

class ChunkStoreTest : public ::testing::Test {
protected:
    ChunkStore open_store() {
        DriveSecret secret{};
        secret[0] = std::byte{42};
        auto store = ChunkStore::open(dir_.path() / "store", ChunkHasher(secret));
        EXPECT_TRUE(store.ok());
        return std::move(store).value();
    }

    static Bytes pattern(std::size_t size, unsigned seed) {
        Bytes data(size);
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = static_cast<std::byte>((i * 31 + seed) & 0xFF);
        }
        return data;
    }

    test::TempDir dir_;
};

TEST_F(ChunkStoreTest, PutReadRoundTrip) {
    ChunkStore store = open_store();
    const Bytes data = pattern(1000, 1);
    auto id = store.put(data);
    ASSERT_TRUE(id.ok());
    EXPECT_TRUE(store.contains(id.value()));
    EXPECT_EQ(store.size(id.value()).value(), 1000u);

    auto all = store.read_verified(id.value());
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all.value(), data);

    Bytes part(100);
    auto n = store.read(id.value(), 950, part);
    ASSERT_TRUE(n.ok());
    ASSERT_EQ(n.value(), 50u);
    EXPECT_TRUE(std::equal(part.begin(), part.begin() + 50, data.begin() + 950));
}

TEST_F(ChunkStoreTest, PutIsIdempotent) {
    ChunkStore store = open_store();
    const Bytes data = pattern(10, 2);
    auto a = store.put(data);
    auto b = store.put(data);
    ASSERT_TRUE(a.ok() && b.ok());
    EXPECT_EQ(a.value(), b.value());
}

TEST_F(ChunkStoreTest, PutVerifiedRejectsMismatch) {
    ChunkStore store = open_store();
    const Bytes data = pattern(64, 3);
    const ChunkId id = store.hasher().hash(data);
    Bytes tampered = data;
    tampered[5] ^= std::byte{1};
    auto st = store.put_verified(id, tampered);
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, Errc::corrupt);
    EXPECT_FALSE(store.contains(id));
    EXPECT_TRUE(store.put_verified(id, data).ok());
    EXPECT_TRUE(store.contains(id));
}

TEST_F(ChunkStoreTest, ReadVerifiedDetectsCorruptionOnDisk) {
    ChunkStore store = open_store();
    auto id = store.put(pattern(64, 4));
    ASSERT_TRUE(id.ok());
    const auto hex = id.value().to_hex();
    const auto path = dir_.path() / "store" / "objects" / hex.substr(2, 2) / hex;
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.good());
        f.seekp(10);
        f.put('X');
    }
    auto read = store.read_verified(id.value());
    ASSERT_FALSE(read.ok());
    EXPECT_EQ(read.error().code, Errc::corrupt);
}

TEST_F(ChunkStoreTest, MissingChunk) {
    ChunkStore store = open_store();
    const ChunkId id = store.hasher().hash(as_bytes("never stored"));
    EXPECT_FALSE(store.contains(id));
    EXPECT_EQ(store.size(id).error().code, Errc::not_found);
    EXPECT_EQ(store.read_verified(id).error().code, Errc::not_found);
    EXPECT_EQ(store.remove(id).error().code, Errc::not_found);
}

TEST_F(ChunkStoreTest, Remove) {
    ChunkStore store = open_store();
    auto id = store.put(pattern(8, 5));
    ASSERT_TRUE(id.ok());
    EXPECT_TRUE(store.remove(id.value()).ok());
    EXPECT_FALSE(store.contains(id.value()));
}

TEST_F(ChunkStoreTest, OpenClearsTempFiles) {
    {
        ChunkStore store = open_store();
    }
    const auto leftover = dir_.path() / "store" / "tmp" / "partial";
    std::ofstream(leftover) << "x";
    ASSERT_TRUE(std::filesystem::exists(leftover));
    ChunkStore store = open_store();
    EXPECT_FALSE(std::filesystem::exists(leftover));
}

TEST_F(ChunkStoreTest, ImportFileSplitsIntoBlocks) {
    ChunkStore store = open_store();
    const std::size_t size = 2 * kMinBlockSize + 123;
    const Bytes data = pattern(size, 6);
    const auto file = dir_.path() / "input.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
    }
    auto imported = store.import_file(file);
    ASSERT_TRUE(imported.ok()) << imported.error().message;
    EXPECT_EQ(imported.value().size, size);
    EXPECT_EQ(imported.value().block_size, kMinBlockSize);
    ASSERT_EQ(imported.value().chunks.size(), 3u);

    Bytes rebuilt;
    for (const ChunkId& id : imported.value().chunks) {
        auto chunk = store.read_verified(id);
        ASSERT_TRUE(chunk.ok());
        rebuilt.insert(rebuilt.end(), chunk.value().begin(), chunk.value().end());
    }
    EXPECT_EQ(rebuilt, data);
}

TEST_F(ChunkStoreTest, ImportEmptyFile) {
    ChunkStore store = open_store();
    const auto file = dir_.path() / "empty.bin";
    std::ofstream(file).close();
    auto imported = store.import_file(file);
    ASSERT_TRUE(imported.ok());
    EXPECT_EQ(imported.value().size, 0u);
    EXPECT_TRUE(imported.value().chunks.empty());
}

}  // namespace
}  // namespace ghostdrive::core
