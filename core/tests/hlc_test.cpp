#include "ghostdrive/core/hlc.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::core {
namespace {

TEST(HybridClock, MonotonicWhenPhysicalClockStalls) {
    std::uint64_t pt = 1000;
    HybridClock clock([&] { return pt; });
    const auto a = clock.now();
    const auto b = clock.now();
    EXPECT_LT(a, b);
    EXPECT_EQ(HybridClock::physical_ms(a), 1000u);
    EXPECT_EQ(HybridClock::logical(b), HybridClock::logical(a) + 1);
}

TEST(HybridClock, MonotonicWhenPhysicalClockGoesBack) {
    std::uint64_t pt = 5000;
    HybridClock clock([&] { return pt; });
    const auto a = clock.now();
    pt = 1000;
    const auto b = clock.now();
    EXPECT_LT(a, b);
    EXPECT_EQ(HybridClock::physical_ms(b), 5000u);
}

TEST(HybridClock, FollowsPhysicalClockForward) {
    std::uint64_t pt = 1000;
    HybridClock clock([&] { return pt; });
    static_cast<void>(clock.now());
    pt = 2000;
    const auto t = clock.now();
    EXPECT_EQ(HybridClock::physical_ms(t), 2000u);
    EXPECT_EQ(HybridClock::logical(t), 0u);
}

TEST(HybridClock, UpdateMovesPastRemote) {
    std::uint64_t pt = 1000;
    HybridClock clock([&] { return pt; });
    const std::uint64_t remote = (9000ull << 16) | 7;
    const auto t = clock.update(remote);
    EXPECT_GT(t, remote);
    EXPECT_GT(clock.now(), t);
}

TEST(HybridClock, LogicalOverflowBorrowsNextMillisecond) {
    std::uint64_t pt = 1000;
    HybridClock clock([&] { return pt; });
    std::uint64_t last = 0;
    for (int i = 0; i < 70000; ++i) {
        const auto t = clock.now();
        ASSERT_GT(t, last);
        last = t;
    }
    EXPECT_GT(HybridClock::physical_ms(last), 1000u);
}

}  // namespace
}  // namespace ghostdrive::core
