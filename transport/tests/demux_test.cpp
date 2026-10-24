#include "ghostdrive/transport/demux.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace ghostdrive::transport {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (auto v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

TEST(Demux, QuicHeadersHaveTheFixedBit) {
    EXPECT_EQ(classify_packet(bytes({0xC0, 0, 0, 0, 1})), PacketKind::quic);  // long header
    EXPECT_EQ(classify_packet(bytes({0x40, 1, 2, 3})), PacketKind::quic);     // short header
    EXPECT_EQ(classify_packet(bytes({0x7F})), PacketKind::quic);
}

TEST(Demux, MagicPrefixIsOurs) {
    std::vector<std::byte> packet(kDatagramMagic.begin(), kDatagramMagic.end());
    packet.push_back(std::byte{0x42});
    EXPECT_EQ(classify_packet(packet), PacketKind::ghostdrive);
}

TEST(Demux, MagicFirstByteHasFixedBitClear) {
    EXPECT_EQ(std::to_integer<unsigned>(kDatagramMagic[0]) & 0x40u, 0u);
}

TEST(Demux, OtherTrafficIsUnknown) {
    EXPECT_EQ(classify_packet({}), PacketKind::unknown);
    EXPECT_EQ(classify_packet(bytes({0x00, 0x01, 0x00, 0x00})), PacketKind::unknown);  // STUN
    EXPECT_EQ(classify_packet(bytes({0x16, 0xFE, 0xFD})), PacketKind::unknown);        // DTLS
    // Truncated or altered magic.
    EXPECT_EQ(classify_packet(bytes({0x0D, 'G', 'D'})), PacketKind::unknown);
    EXPECT_EQ(classify_packet(bytes({0x0D, 'G', 'X', 0x01})), PacketKind::unknown);
}

}  // namespace
}  // namespace ghostdrive::transport
