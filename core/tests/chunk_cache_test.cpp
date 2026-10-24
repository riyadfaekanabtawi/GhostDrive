#include "ghostdrive/core/chunk_cache.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::core {
namespace {

ChunkId id_with(std::uint8_t fill) {
    ChunkId id;
    id.digest.fill(std::byte{fill});
    return id;
}

SharedChunk chunk_of(std::size_t size) {
    return std::make_shared<const Bytes>(size);
}

TEST(ChunkCache, GetReturnsWhatWasPut) {
    ChunkCache cache(1000);
    auto chunk = chunk_of(100);
    cache.put(id_with(1), chunk);
    EXPECT_EQ(cache.get(id_with(1)), chunk);
    EXPECT_EQ(cache.get(id_with(2)), nullptr);
    EXPECT_EQ(cache.size_bytes(), 100u);
}

TEST(ChunkCache, EvictsLeastRecentlyUsed) {
    ChunkCache cache(300);
    cache.put(id_with(1), chunk_of(100));
    cache.put(id_with(2), chunk_of(100));
    cache.put(id_with(3), chunk_of(100));
    ASSERT_NE(cache.get(id_with(1)), nullptr);  // 1 becomes most recent; 2 is now oldest
    cache.put(id_with(4), chunk_of(100));
    EXPECT_TRUE(cache.contains(id_with(1)));
    EXPECT_FALSE(cache.contains(id_with(2)));
    EXPECT_TRUE(cache.contains(id_with(3)));
    EXPECT_TRUE(cache.contains(id_with(4)));
    EXPECT_EQ(cache.size_bytes(), 300u);
}

TEST(ChunkCache, EvictedChunkStaysAliveForHolders) {
    ChunkCache cache(100);
    cache.put(id_with(1), chunk_of(100));
    auto held = cache.get(id_with(1));
    cache.put(id_with(2), chunk_of(100));
    EXPECT_FALSE(cache.contains(id_with(1)));
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(held->size(), 100u);
}

TEST(ChunkCache, OversizedChunkIsNotCached) {
    ChunkCache cache(100);
    cache.put(id_with(1), chunk_of(50));
    cache.put(id_with(2), chunk_of(101));
    EXPECT_FALSE(cache.contains(id_with(2)));
    EXPECT_TRUE(cache.contains(id_with(1)));
}

TEST(ChunkCache, ReplacingAChunkKeepsAccountingRight) {
    ChunkCache cache(1000);
    cache.put(id_with(1), chunk_of(100));
    cache.put(id_with(1), chunk_of(300));
    EXPECT_EQ(cache.size_bytes(), 300u);
    cache.erase(id_with(1));
    EXPECT_EQ(cache.size_bytes(), 0u);
    EXPECT_FALSE(cache.contains(id_with(1)));
}

}  // namespace
}  // namespace ghostdrive::core
