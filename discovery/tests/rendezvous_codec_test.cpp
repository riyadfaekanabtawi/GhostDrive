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

TEST(RendezvousCodec, RegisterRoundTripsWithAdmissionsAndHiddenGroups) {
    Register reg{group(std::byte{1}),
                 {group(std::byte{2}), group(std::byte{5}), group(std::byte{6})},
                 {group(std::byte{3}), group(std::byte{4})}};
    auto decoded = decode_register(payload_of(encode(reg)));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().group, reg.group);
    EXPECT_EQ(decoded.value().admissions, reg.admissions);
    EXPECT_EQ(decoded.value().hidden, reg.hidden);

    Register full{group(std::byte{1}), std::vector<GroupId>(kMaxAdmissions, group(std::byte{2})),
                  std::vector<GroupId>(kMaxHiddenGroups, group(std::byte{3}))};
    auto big = decode_register(payload_of(encode(full)));
    ASSERT_TRUE(big);
    EXPECT_EQ(big.value().admissions.size(), kMaxAdmissions);
    EXPECT_EQ(big.value().hidden.size(), kMaxHiddenGroups);
}

TEST(RendezvousCodec, AdmissionIdsDifferFromGroups) {
    core::DriveSecret key{};
    key.fill(std::byte{9});
    auto admission = rendezvous_admission(key);
    auto group_id = rendezvous_group(key);
    ASSERT_TRUE(admission && group_id);
    EXPECT_NE(admission.value(), group_id.value()) << "the operator could join the group";
    key[0] = std::byte{8};
    EXPECT_NE(rendezvous_admission(key).value(), admission.value());
}

TEST(RendezvousCodec, AnOlderClientsRegisterIsItsOwnAdmission) {
    auto g = group(std::byte{5});
    auto decoded = decode_register(std::span<const std::byte>(g));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().group, g);
    EXPECT_EQ(decoded.value().admissions, std::vector<GroupId>{g});
    EXPECT_TRUE(decoded.value().hidden.empty());
}

TEST(RendezvousCodec, TheFixedFormStillDecodes) {
    // group | admission | count | hidden, as clients sent before admission lists; it carries
    // the generation-0 ID, the last admission.
    Register reg{
        group(std::byte{1}), {group(std::byte{7}), group(std::byte{2})}, {group(std::byte{3})}};
    auto frame = encode_fixed_register(reg);
    auto payload = payload_of(frame);
    ASSERT_EQ(payload.size(), 2 * kGroupIdSize + 1 + kGroupIdSize);
    EXPECT_TRUE(std::equal(payload.begin() + kGroupIdSize, payload.begin() + 2 * kGroupIdSize,
                           reg.admissions.back().begin()));
    auto decoded = decode_register(payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().group, reg.group);
    EXPECT_EQ(decoded.value().admissions, std::vector<GroupId>{group(std::byte{2})});
    EXPECT_EQ(decoded.value().hidden, reg.hidden);
}

TEST(RendezvousCodec, MalformedRegistersAreRejected) {
    Register reg{group(std::byte{1}), {group(std::byte{2})}, {group(std::byte{3})}};
    auto frame = encode(reg);
    auto payload = payload_of(frame);
    EXPECT_FALSE(decode_register(payload.first(payload.size() - 1)));
    EXPECT_FALSE(decode_register(payload.first(kGroupIdSize + 1)));                 // no admission
    EXPECT_FALSE(decode_register(payload.first(kGroupIdSize + 1 + kGroupIdSize)));  // no hidden
    core::Bytes longer(payload.begin(), payload.end());
    longer.resize(longer.size() + kGroupIdSize);
    EXPECT_FALSE(decode_register(longer)) << "trailing bytes";

    core::Bytes no_admission(payload.begin(), payload.begin() + kGroupIdSize);
    no_admission.push_back(std::byte{0});
    no_admission.push_back(std::byte{0});
    EXPECT_FALSE(decode_register(no_admission));

    core::Bytes too_many_admissions(payload.begin(), payload.begin() + kGroupIdSize);
    too_many_admissions.push_back(static_cast<std::byte>(kMaxAdmissions + 1));
    too_many_admissions.resize(too_many_admissions.size() + (kMaxAdmissions + 1) * kGroupIdSize);
    too_many_admissions.push_back(std::byte{0});
    EXPECT_FALSE(decode_register(too_many_admissions));

    core::Bytes too_many_hidden(payload.begin(), payload.begin() + kGroupIdSize);
    too_many_hidden.push_back(std::byte{1});
    too_many_hidden.resize(too_many_hidden.size() + kGroupIdSize);
    too_many_hidden.push_back(static_cast<std::byte>(kMaxHiddenGroups + 1));
    too_many_hidden.resize(too_many_hidden.size() + (kMaxHiddenGroups + 1) * kGroupIdSize);
    EXPECT_FALSE(decode_register(too_many_hidden));

    // The fixed form keeps its old limits.
    auto fixed = encode_fixed_register(reg);
    auto fixed_payload = payload_of(fixed);
    EXPECT_FALSE(decode_register(fixed_payload.first(2 * kGroupIdSize)));  // no hidden count
    core::Bytes fixed_too_many(fixed_payload.begin(), fixed_payload.begin() + 2 * kGroupIdSize);
    fixed_too_many.push_back(static_cast<std::byte>(kMaxHiddenGroups + 1));
    fixed_too_many.resize(fixed_too_many.size() + (kMaxHiddenGroups + 1) * kGroupIdSize);
    EXPECT_FALSE(decode_register(fixed_too_many));

    // The encoder never sends more than the limits.
    Register big{group(std::byte{1}), std::vector<GroupId>(kMaxAdmissions + 2, group(std::byte{2})),
                 std::vector<GroupId>(kMaxHiddenGroups + 2, group(std::byte{3}))};
    auto capped = decode_register(payload_of(encode(big)));
    ASSERT_TRUE(capped);
    EXPECT_EQ(capped.value().admissions.size(), kMaxAdmissions);
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
