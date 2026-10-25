#include "ghostdrive/core/chunk_id.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <unordered_set>

#include "ghostdrive/core/bytes.hpp"

namespace ghostdrive::core {
namespace {

DriveSecret secret_from(std::string_view text) {
    DriveSecret secret{};
    std::memcpy(secret.data(), text.data(), std::min(text.size(), secret.size()));
    return secret;
}

// BLAKE3 test_vectors.json: keyed_hash of the empty input under the official test key.
TEST(ChunkId, MatchesBlake3KeyedVector) {
    const ChunkHasher hasher(secret_from("whats the Elvish word for friend"));
    const ChunkId id = hasher.hash({});
    EXPECT_EQ(to_hex(id.digest),
              "92b2b75604ed3c761f9d6f62392c8a9227ad0ea3f09573e783f1498a4ed60d26");
}

TEST(ChunkId, DifferentSecretsGiveDifferentIds) {
    const ChunkHasher a(secret_from("drive secret A................."));
    const ChunkHasher b(secret_from("drive secret B................."));
    const auto data = as_bytes("same chunk contents");
    EXPECT_EQ(a.hash(data), a.hash(data));
    EXPECT_NE(a.hash(data), b.hash(data));
}

TEST(ChunkId, BytesRoundTrip) {
    const ChunkHasher hasher(secret_from("k"));
    const ChunkId id = hasher.hash(as_bytes("abc"));
    const auto bytes = id.to_bytes();
    EXPECT_EQ(bytes[0], std::byte{1});
    auto back = ChunkId::from_bytes(bytes);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), id);
    EXPECT_EQ(id.to_hex().size(), kChunkIdSize * 2);
    EXPECT_EQ(id.short_hex().size(), 8u);
    EXPECT_EQ(id.to_hex().substr(2, 8), id.short_hex()) << "the digest after the tag";
}

TEST(ChunkId, FromBytesRejectsBadInput) {
    std::array<std::byte, kChunkIdSize> bytes{};
    bytes[0] = std::byte{0x7F};
    EXPECT_FALSE(ChunkId::from_bytes(bytes).ok());
    EXPECT_FALSE(ChunkId::from_bytes(std::span(bytes).first(10)).ok());
}

TEST(ChunkId, UsableAsHashKey) {
    const ChunkHasher hasher(secret_from("k"));
    std::unordered_set<ChunkId> set;
    set.insert(hasher.hash(as_bytes("a")));
    set.insert(hasher.hash(as_bytes("b")));
    set.insert(hasher.hash(as_bytes("a")));
    EXPECT_EQ(set.size(), 2u);
}

}  // namespace
}  // namespace ghostdrive::core
