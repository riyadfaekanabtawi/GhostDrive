#include "ghostdrive/core/version_vector.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::core {
namespace {

using Order = VersionVector::Order;

TEST(VersionVector, CompareBasics) {
    VersionVector a;
    VersionVector b;
    EXPECT_EQ(a.compare(b), Order::equal);

    a.bump(1);
    EXPECT_EQ(a.compare(b), Order::after);
    EXPECT_EQ(b.compare(a), Order::before);

    b = a;
    b.bump(2);
    EXPECT_EQ(a.compare(b), Order::before);

    a.bump(1);
    EXPECT_EQ(a.compare(b), Order::concurrent);
    EXPECT_EQ(b.compare(a), Order::concurrent);
}

TEST(VersionVector, MergeDominatesBoth) {
    VersionVector a;
    a.bump(1);
    a.bump(1);
    VersionVector b;
    b.bump(2);
    b.bump(3);
    const VersionVector m = a.merged(b);
    EXPECT_EQ(m.get(1), 2u);
    EXPECT_EQ(m.get(2), 1u);
    EXPECT_EQ(m.get(3), 1u);
    EXPECT_EQ(m.compare(a), Order::after);
    EXPECT_EQ(m.compare(b), Order::after);
    EXPECT_EQ(a.merged(b), b.merged(a));
}

TEST(VersionVector, KeepsDevicesSorted) {
    VersionVector v;
    v.bump(30);
    v.bump(10);
    v.bump(20);
    ASSERT_EQ(v.counters().size(), 3u);
    EXPECT_EQ(v.counters()[0].device, 10u);
    EXPECT_EQ(v.counters()[2].device, 30u);
}

TEST(VersionVector, SerializeRoundTrip) {
    VersionVector v;
    v.bump(0xFFFFFFFFFFFFFFFFull);
    v.bump(7);
    v.bump(7);
    auto parsed = VersionVector::parse(v.serialize());
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value(), v);

    auto empty = VersionVector::parse(VersionVector{}.serialize());
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty.value().counters().empty());
}

TEST(VersionVector, ParseRejectsMalformed) {
    EXPECT_FALSE(VersionVector::parse({}).ok());

    VersionVector v;
    v.bump(1);
    Bytes truncated = v.serialize();
    truncated.pop_back();
    EXPECT_FALSE(VersionVector::parse(truncated).ok());

    Bytes zero = v.serialize();
    for (std::size_t i = 12; i < 20; ++i) zero[i] = std::byte{0};
    EXPECT_FALSE(VersionVector::parse(zero).ok());

    VersionVector two;
    two.bump(1);
    two.bump(2);
    Bytes unsorted = two.serialize();
    std::swap_ranges(unsorted.begin() + 4, unsorted.begin() + 20, unsorted.begin() + 20);
    EXPECT_FALSE(VersionVector::parse(unsorted).ok());
}

}  // namespace
}  // namespace ghostdrive::core
