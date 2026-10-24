#include "ghostdrive/core/chunking.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::core {
namespace {

constexpr std::uint64_t kMiB = 1024 * 1024;

TEST(Chunking, BlockSizeBoundaries) {
    EXPECT_EQ(block_size_for(0), kMinBlockSize);
    EXPECT_EQ(block_size_for(1), kMinBlockSize);
    // 2000 blocks of 128 KiB is exactly 250 MiB.
    EXPECT_EQ(block_size_for(250 * kMiB), kMinBlockSize);
    EXPECT_EQ(block_size_for(250 * kMiB + 1), 256u * 1024);
    EXPECT_EQ(block_size_for(500 * kMiB), 256u * 1024);
    EXPECT_EQ(block_size_for(500 * kMiB + 1), 512u * 1024);
    EXPECT_EQ(block_size_for(16000 * kMiB), 8 * kMiB);
    EXPECT_EQ(block_size_for(16000 * kMiB + 1), kMaxBlockSize);
    EXPECT_EQ(block_size_for(1ull << 40), kMaxBlockSize);
}

TEST(Chunking, ChunkCount) {
    EXPECT_EQ(chunk_count(0, kMinBlockSize), 0u);
    EXPECT_EQ(chunk_count(1, kMinBlockSize), 1u);
    EXPECT_EQ(chunk_count(kMinBlockSize, kMinBlockSize), 1u);
    EXPECT_EQ(chunk_count(kMinBlockSize + 1, kMinBlockSize), 2u);
    EXPECT_EQ(chunk_count(10, 0), 0u);
}

TEST(Chunking, RangeToChunks) {
    const std::uint32_t b = 4 * kMiB;
    const std::uint64_t size = 1000 * kMiB;

    auto one = chunks_for_range(300 * kMiB, kMiB, size, b);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->first, 75u);
    EXPECT_EQ(one->last, 75u);

    auto straddle = chunks_for_range(4 * kMiB - 1, 2, size, b);
    ASSERT_TRUE(straddle.has_value());
    EXPECT_EQ(straddle->first, 0u);
    EXPECT_EQ(straddle->last, 1u);

    auto clipped = chunks_for_range(size - 10, 1000, size, b);
    ASSERT_TRUE(clipped.has_value());
    EXPECT_EQ(clipped->last, chunk_count(size, b) - 1);

    auto huge = chunks_for_range(1, UINT64_MAX, size, b);
    ASSERT_TRUE(huge.has_value());
    EXPECT_EQ(huge->first, 0u);
    EXPECT_EQ(huge->last, chunk_count(size, b) - 1);
}

TEST(Chunking, EmptyRanges) {
    EXPECT_FALSE(chunks_for_range(0, 0, 100, kMinBlockSize).has_value());
    EXPECT_FALSE(chunks_for_range(100, 10, 100, kMinBlockSize).has_value());
    EXPECT_FALSE(chunks_for_range(0, 10, 100, 0).has_value());
}

}  // namespace
}  // namespace ghostdrive::core
