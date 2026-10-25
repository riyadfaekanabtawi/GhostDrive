// The app's parser against the daemon's real `status --json` writer, so a change to one that
// the other does not follow fails here.

#include <gtest/gtest.h>

#include "ghostdrive/app/status.hpp"
#include "status_json.hpp"

namespace ghostdrive::app {
namespace {

TEST(StatusContract, ParsesWhatTheDaemonWrites) {
    daemon::ServiceStatus s;
    s.device_name = "my \"desk\"\n";
    s.fingerprint = "AAAA-BBBB";
    s.port = 7373;
    s.rendezvous = "rv.example:7374";
    s.observed = asio::ip::udp::endpoint(asio::ip::make_address("203.0.113.5"), 4000);
    s.removed_by = "laptop";
    daemon::PeerStatus a;
    a.key.fill(std::byte{1});
    a.name = "phone\xe2\x80\xae";
    a.address = "192.0.2.1:7373";
    a.connected = true;
    a.relayed = true;
    daemon::PeerStatus b;
    b.key.fill(std::byte{2});
    b.name = "nas";
    s.peers = {a, b};

    auto parsed = parse_status(daemon::format_status_json(s, "/home/me/GhostDrive"));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->device, "my \"desk\"?");
    EXPECT_EQ(parsed->id, "AAAA-BBBB");
    EXPECT_EQ(parsed->mountpoint, "/home/me/GhostDrive");
    EXPECT_EQ(parsed->port, 7373);
    ASSERT_TRUE(parsed->rendezvous);
    EXPECT_EQ(parsed->rendezvous->address, "rv.example:7374");
    EXPECT_TRUE(parsed->rendezvous->connected);
    EXPECT_EQ(parsed->rendezvous->observed, "203.0.113.5:4000");
    EXPECT_EQ(parsed->removed_by, "laptop");
    EXPECT_FALSE(parsed->discovery_error);
    ASSERT_EQ(parsed->peers.size(), 2u);
    EXPECT_EQ(parsed->peers[0].name, "phone?");
    EXPECT_EQ(parsed->peers[0].id, core::fingerprint(a.key));
    EXPECT_EQ(peer_state(parsed->peers[0]), PeerState::relayed);
    EXPECT_EQ(parsed->peers[1].name, "nas");
    EXPECT_FALSE(parsed->peers[1].address);
    EXPECT_EQ(peer_state(parsed->peers[1]), PeerState::offline);
}

TEST(StatusContract, ParsesAnIdleDaemon) {
    daemon::ServiceStatus s;
    s.device_name = "desk";
    s.fingerprint = "AAAA";
    auto parsed = parse_status(daemon::format_status_json(s, ""));
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(parsed->mountpoint);
    EXPECT_FALSE(parsed->rendezvous);
    EXPECT_TRUE(parsed->peers.empty());
}

}  // namespace
}  // namespace ghostdrive::app
