// Needs the system's mDNS responder (mDNSResponder, or avahi-daemon on Linux); skips without it.
#include "ghostdrive/discovery/lan_discovery.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <asio/io_context.hpp>
#include <vector>

namespace ghostdrive::discovery {
namespace {

using namespace std::chrono_literals;

template <typename Done>
bool run_until(asio::io_context& io, Done done, std::chrono::milliseconds limit = 20s) {
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        io.restart();
        io.run_for(20ms);
    }
    return true;
}

bool saw(const std::vector<LanSighting>& seen, const std::string& instance, const std::string& id,
         std::uint16_t port) {
    return std::any_of(seen.begin(), seen.end(), [&](const LanSighting& s) {
        return s.instance == instance && s.id == id && s.endpoint.port() == port;
    });
}

TEST(LanDiscovery, DevicesSeeEachOtherButNotThemselves) {
    asio::io_context io;
    std::vector<LanSighting> seen_a, seen_b, seen_c;
    std::string id_b = "bbbb";
    auto a = LanDiscovery::start(
        io.get_executor(), 41001, [] { return std::string("aaaa"); },
        [&](const LanSighting& s) { seen_a.push_back(s); });
    if (!a) GTEST_SKIP() << "no mDNS responder: " << a.error().message;
    auto b = LanDiscovery::start(
        io.get_executor(), 41002, [&] { return id_b; },
        [&](const LanSighting& s) { seen_b.push_back(s); });
    ASSERT_TRUE(b) << b.error().message;
    const auto& name_a = a.value()->instance_name();
    const auto& name_b = b.value()->instance_name();

    ASSERT_TRUE(run_until(io, [&] {
        return saw(seen_a, name_b, "bbbb", 41002) && saw(seen_b, name_a, "aaaa", 41001);
    })) << "the two advertisements were not resolved";
    for (const auto& s : seen_a) EXPECT_NE(s.instance, name_a);
    for (const auto& s : seen_b) EXPECT_NE(s.instance, name_b);

    // A device that starts browsing after the ID changed resolves the new one, once the
    // responder's cache has caught up with the re-announcement.
    id_b = "cccc";
    b.value()->refresh();
    bool refreshed = false;
    for (int attempt = 0; attempt < 20 && !refreshed; ++attempt) {
        auto c = LanDiscovery::start(
            io.get_executor(), 41003, [] { return std::string("dddd"); },
            [&](const LanSighting& s) { seen_c.push_back(s); });
        ASSERT_TRUE(c) << c.error().message;
        refreshed = run_until(io, [&] { return saw(seen_c, name_b, "cccc", 41002); }, 1s);
    }
    EXPECT_TRUE(refreshed) << "the refreshed ID was not advertised";
}

}  // namespace
}  // namespace ghostdrive::discovery
