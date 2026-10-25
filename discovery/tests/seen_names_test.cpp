#include "ghostdrive/discovery/seen_names.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::discovery {
namespace {

using namespace std::chrono_literals;

TEST(SeenNames, ANameIsResolvedOnceUntilItExpires) {
    SeenNames seen(4, 3min);
    auto t0 = SeenNames::Clock::time_point{} + 1h;
    EXPECT_TRUE(seen.due("a", t0));
    seen.remember("a", t0);
    EXPECT_FALSE(seen.due("a", t0 + 2min));
    // Expired: browsed again, it is resolved again (a rotated ID or a new address).
    EXPECT_TRUE(seen.due("a", t0 + 3min));
    seen.remember("a", t0 + 3min);
    EXPECT_FALSE(seen.due("a", t0 + 4min));
}

TEST(SeenNames, AFullTableDropsTheOldestInsteadOfRefusing) {
    SeenNames seen(3, 3min);
    auto t0 = SeenNames::Clock::time_point{} + 1h;
    seen.remember("old", t0);
    seen.remember("b", t0 + 1s);
    seen.remember("c", t0 + 2s);
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_TRUE(seen.due("new", t0 + 3s));
    seen.remember("new", t0 + 3s);
    EXPECT_EQ(seen.size(), 3u);
    EXPECT_FALSE(seen.due("new", t0 + 4s));
    EXPECT_TRUE(seen.due("old", t0 + 4s)) << "the oldest entry should have been dropped";
    EXPECT_FALSE(seen.due("b", t0 + 4s));
    EXPECT_FALSE(seen.due("c", t0 + 4s));
}

TEST(SeenNames, AFloodCannotShutOutANewName) {
    SeenNames seen(256, 3min);
    auto t0 = SeenNames::Clock::time_point{} + 1h;
    for (int i = 0; i < 1000; ++i) seen.remember("flood-" + std::to_string(i), t0);
    EXPECT_EQ(seen.size(), 256u);
    EXPECT_TRUE(seen.due("peer", t0 + 1s));
    seen.remember("peer", t0 + 1s);
    EXPECT_FALSE(seen.due("peer", t0 + 2s));
}

TEST(SeenNames, ForgottenNamesAreDueAgain) {
    SeenNames seen(4, 3min);
    auto t0 = SeenNames::Clock::time_point{} + 1h;
    seen.remember("a", t0);
    seen.forget("a");
    EXPECT_TRUE(seen.due("a", t0 + 1s));
}

}  // namespace
}  // namespace ghostdrive::discovery
