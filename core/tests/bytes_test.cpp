#include "ghostdrive/core/bytes.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::core {
namespace {

TEST(Bytes, HexRoundTrip) {
    const Bytes data{std::byte{0x00}, std::byte{0xAB}, std::byte{0x7F}, std::byte{0xFF}};
    EXPECT_EQ(to_hex(data), "00ab7fff");
    auto back = from_hex("00AB7fff");
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, data);
}

TEST(Bytes, FromHexRejectsBadInput) {
    EXPECT_FALSE(from_hex("abc").has_value());
    EXPECT_FALSE(from_hex("zz").has_value());
}

// RFC 4648 section 10 test vectors, without padding.
TEST(Bytes, Base32Rfc4648Vectors) {
    EXPECT_EQ(base32_encode(as_bytes("")), "");
    EXPECT_EQ(base32_encode(as_bytes("f")), "MY");
    EXPECT_EQ(base32_encode(as_bytes("fo")), "MZXQ");
    EXPECT_EQ(base32_encode(as_bytes("foo")), "MZXW6");
    EXPECT_EQ(base32_encode(as_bytes("foob")), "MZXW6YQ");
    EXPECT_EQ(base32_encode(as_bytes("fooba")), "MZXW6YTB");
    EXPECT_EQ(base32_encode(as_bytes("foobar")), "MZXW6YTBOI");
}

TEST(Bytes, RandomBytesFillsBuffer) {
    std::array<std::byte, 32> a{};
    std::array<std::byte, 32> b{};
    ASSERT_TRUE(random_bytes(a).ok());
    ASSERT_TRUE(random_bytes(b).ok());
    EXPECT_NE(a, b);
}

}  // namespace
}  // namespace ghostdrive::core
