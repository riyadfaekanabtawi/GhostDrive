#include "ghostdrive/discovery/rendezvous_codec.hpp"

#include <gtest/gtest.h>

#include "ghostdrive/transport/frame.hpp"

namespace ghostdrive::discovery {
namespace {

std::span<const std::byte> payload_of(const core::Bytes& frame) {
    return std::span(frame).subspan(transport::wire::kHeaderSize);
}

GroupId group(std::byte fill) {
    GroupId g{};
    g.fill(fill);
    return g;
}

TEST(RendezvousCodec, RegisterRoundTripsWithHiddenGroups) {
    Register reg{
        group(std::byte{1}), group(std::byte{2}), {group(std::byte{3}), group(std::byte{4})}};
    auto decoded = decode_register(payload_of(encode(reg)));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().group, reg.group);
    EXPECT_EQ(decoded.value().admission, reg.admission);
    EXPECT_EQ(decoded.value().hidden, reg.hidden);
}

TEST(RendezvousCodec, AnOlderClientsRegisterIsItsOwnAdmission) {
    auto g = group(std::byte{5});
    auto decoded = decode_register(std::span<const std::byte>(g));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().group, g);
    EXPECT_EQ(decoded.value().admission, g);
    EXPECT_TRUE(decoded.value().hidden.empty());
}

TEST(RendezvousCodec, MalformedRegistersAreRejected) {
    Register reg{group(std::byte{1}), group(std::byte{2}), {group(std::byte{3})}};
    auto frame = encode(reg);
    auto payload = payload_of(frame);
    EXPECT_FALSE(decode_register(payload.first(payload.size() - 1)));
    EXPECT_FALSE(decode_register(payload.first(2 * kGroupIdSize)));  // no hidden count
    core::Bytes too_many(payload.begin(), payload.begin() + 2 * kGroupIdSize);
    too_many.push_back(static_cast<std::byte>(kMaxHiddenGroups + 1));
    too_many.resize(too_many.size() + (kMaxHiddenGroups + 1) * kGroupIdSize);
    EXPECT_FALSE(decode_register(too_many));
    // The encoder never sends more than the limit.
    Register big{group(std::byte{1}), group(std::byte{2}),
                 std::vector<GroupId>(kMaxHiddenGroups + 2, group(std::byte{3}))};
    auto capped = decode_register(payload_of(encode(big)));
    ASSERT_TRUE(capped);
    EXPECT_EQ(capped.value().hidden.size(), kMaxHiddenGroups);
}

TEST(RendezvousCodec, RegisteredCarriesOnlyTheObservedAddress) {
    asio::ip::udp::endpoint observed(asio::ip::make_address("192.0.2.7"), 4242);
    auto frame = encode(Registered{observed});
    auto decoded = decode_registered(payload_of(frame));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().observed, observed);
    // The 16-byte relay token that used to follow is gone; trailing bytes are malformed.
    core::Bytes longer(payload_of(frame).begin(), payload_of(frame).end());
    longer.resize(longer.size() + 16);
    EXPECT_FALSE(decode_registered(longer));
}

}  // namespace
}  // namespace ghostdrive::discovery
