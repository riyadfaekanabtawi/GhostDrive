#include "ghostdrive/transport/frame.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::transport::wire {
namespace {

core::ChunkId sample_id() {
    core::ChunkId id;
    for (std::size_t i = 0; i < id.digest.size(); ++i) id.digest[i] = static_cast<std::byte>(i);
    return id;
}

std::span<const std::byte, kHeaderSize> header_of(const core::Bytes& frame) {
    return std::span<const std::byte, kHeaderSize>(frame.data(), kHeaderSize);
}

TEST(Frame, HeaderIsLittleEndianWithVersionAndType) {
    auto header = encode_header(FrameType::chunk_data, 0x00A1B2C3);
    EXPECT_EQ(header[0], std::byte{0xC3});
    EXPECT_EQ(header[1], std::byte{0xB2});
    EXPECT_EQ(header[2], std::byte{0xA1});
    EXPECT_EQ(header[3], std::byte{0x00});
    EXPECT_EQ(header[4], std::byte{kVersion});
    EXPECT_EQ(header[5], std::byte{2});

    auto decoded = decode_header(header);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value().type, FrameType::chunk_data);
    EXPECT_EQ(decoded.value().length, 0x00A1B2C3u);
}

TEST(Frame, RejectsUnknownVersion) {
    auto header = encode_header(FrameType::get_chunk, 33);
    header[4] = std::byte{2};
    auto decoded = decode_header(header);
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, core::Errc::protocol);
}

TEST(Frame, RejectsUnknownType) {
    auto header = encode_header(FrameType::get_chunk, 33);
    header[5] = std::byte{0};
    EXPECT_FALSE(decode_header(header));
    header[5] = std::byte{4};
    EXPECT_FALSE(decode_header(header));
}

TEST(Frame, RejectsOversizedPayload) {
    EXPECT_TRUE(decode_header(encode_header(FrameType::chunk_data, kMaxPayload)));
    EXPECT_FALSE(decode_header(encode_header(FrameType::chunk_data, kMaxPayload + 1)));
    EXPECT_FALSE(decode_header(encode_header(FrameType::chunk_data, 100), 99));
}

TEST(Frame, GetChunkRoundTrips) {
    auto frame = encode_get_chunk(sample_id());
    ASSERT_EQ(frame.size(), kHeaderSize + core::kChunkIdSize);
    auto header = decode_header(header_of(frame));
    ASSERT_TRUE(header);
    EXPECT_EQ(header.value().type, FrameType::get_chunk);
    EXPECT_EQ(header.value().length, core::kChunkIdSize);

    auto id = decode_get_chunk(std::span(frame).subspan(kHeaderSize));
    ASSERT_TRUE(id) << id.error().message;
    EXPECT_EQ(id.value(), sample_id());
}

TEST(Frame, GetChunkRejectsWrongSizeAndAlgorithm) {
    auto frame = encode_get_chunk(sample_id());
    auto payload = std::span(frame).subspan(kHeaderSize);
    EXPECT_FALSE(decode_get_chunk(payload.first(payload.size() - 1)));

    core::Bytes bad(payload.begin(), payload.end());
    bad[0] = std::byte{0xEE};  // unknown hash algorithm tag
    auto id = decode_get_chunk(bad);
    ASSERT_FALSE(id);
    EXPECT_EQ(id.error().code, core::Errc::protocol);
}

TEST(Frame, ErrorRoundTrips) {
    auto frame = encode_error(ErrorCode::not_found);
    auto header = decode_header(header_of(frame));
    ASSERT_TRUE(header);
    EXPECT_EQ(header.value().type, FrameType::error);
    auto code = decode_error(std::span(frame).subspan(kHeaderSize));
    ASSERT_TRUE(code);
    EXPECT_EQ(code.value(), ErrorCode::not_found);
    EXPECT_FALSE(decode_error(std::span(frame).subspan(kHeaderSize, 1)));
}

}  // namespace
}  // namespace ghostdrive::transport::wire
