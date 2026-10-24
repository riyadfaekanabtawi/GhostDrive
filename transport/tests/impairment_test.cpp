#include "ghostdrive/transport/impairment.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::transport {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;

TEST(Impairment, InactiveByDefault) {
    Impairment config;
    EXPECT_FALSE(config.active());
    Impairer impairer(config);
    auto plan = impairer.plan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].count(), 0);
}

TEST(Impairment, TotalLossDropsEverything) {
    Impairment config;
    config.loss = 1.0;
    Impairer impairer(config);
    for (int i = 0; i < 100; ++i) EXPECT_TRUE(impairer.plan().empty());
}

TEST(Impairment, LossRateIsRoughlyAsConfigured) {
    Impairment config;
    config.loss = 0.3;
    config.seed = 7;
    Impairer impairer(config);
    int dropped = 0;
    constexpr int kPackets = 20000;
    for (int i = 0; i < kPackets; ++i) dropped += impairer.plan().empty() ? 1 : 0;
    EXPECT_NEAR(static_cast<double>(dropped) / kPackets, 0.3, 0.02);
}

TEST(Impairment, DuplicateSendsTwoCopies) {
    Impairment config;
    config.duplicate = 1.0;
    Impairer impairer(config);
    EXPECT_EQ(impairer.plan().size(), 2u);
}

TEST(Impairment, DelayAndJitterStayInBounds) {
    Impairment config;
    config.delay = milliseconds(20);
    config.jitter = milliseconds(5);
    Impairer impairer(config);
    for (int i = 0; i < 1000; ++i) {
        auto plan = impairer.plan();
        ASSERT_EQ(plan.size(), 1u);
        EXPECT_GE(plan[0], milliseconds(20));
        EXPECT_LE(plan[0], milliseconds(25));
    }
}

TEST(Impairment, ReorderAddsTheReorderDelay) {
    Impairment config;
    config.reorder = 1.0;
    config.reorder_delay = milliseconds(15);
    Impairer impairer(config);
    EXPECT_EQ(impairer.plan()[0], milliseconds(15));
}

TEST(Impairment, SameSeedReplaysTheSameDecisions) {
    Impairment config;
    config.loss = 0.2;
    config.jitter = microseconds(500);
    config.duplicate = 0.1;
    config.seed = 42;
    Impairer a(config);
    Impairer b(config);
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(a.plan(), b.plan());
}

}  // namespace
}  // namespace ghostdrive::transport
