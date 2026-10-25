#include "ghostdrive/app/status.hpp"

#include <gtest/gtest.h>

#include "ghostdrive/app/json.hpp"

namespace ghostdrive::app {
namespace {

// The shape daemon/src/status_json.hpp writes.
constexpr std::string_view kFull =
    "{\"device\":\"desk\",\"id\":\"AAAA-BBBB\",\"mountpoint\":\"/home/me/GhostDrive\","
    "\"port\":7373,\"rendezvous\":{\"address\":\"rv.example:7374\",\"connected\":true,"
    "\"observed\":\"203.0.113.5:4000\"},\"removed_by\":null,\"discovery_error\":null,"
    "\"peers\":[{\"name\":\"phone\",\"id\":\"CCCC\",\"address\":\"192.0.2.1:7373\","
    "\"online\":true,\"relayed\":true},{\"name\":\"nas\",\"id\":\"DDDD\",\"address\":null,"
    "\"online\":false,\"relayed\":false},{\"name\":\"laptop\",\"id\":\"EEEE\","
    "\"address\":\"192.0.2.2:7373\",\"online\":true,\"relayed\":false}]}\n";

TEST(Json, ParsesValues) {
    auto v = parse_json(" {\"a\":[1,-2,3.5,true,false,null,\"x\"],\"b\":{}} ");
    ASSERT_TRUE(v);
    const JsonValue* a = v->find("a");
    ASSERT_TRUE(a && a->is(JsonValue::Kind::array));
    ASSERT_EQ(a->items.size(), 7u);
    EXPECT_EQ(a->items[0].integer, 1);
    EXPECT_EQ(a->items[1].integer, -2);
    EXPECT_TRUE(a->items[2].is(JsonValue::Kind::number));
    EXPECT_TRUE(a->items[3].boolean);
    EXPECT_TRUE(a->items[5].is(JsonValue::Kind::null));
    EXPECT_EQ(a->items[6].string, "x");
    EXPECT_TRUE(v->find("b")->is(JsonValue::Kind::object));
    EXPECT_EQ(v->find("missing"), nullptr);
}

TEST(Json, DecodesEscapes) {
    auto v = parse_json(R"("a\"b\\c\/d\n\u0041\u00e9\ud83d\ude00\ud800x")");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->string, "a\"b\\c/d\nA\xc3\xa9\xf0\x9f\x98\x80\xef\xbf\xbdx");
}

TEST(Json, RejectsMalformedInput) {
    for (std::string_view bad :
         {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "\"unterminated", "01", "1.", "-", "tru",
          "{} extra", "\"raw\ncontrol\"", "\"bad \\q escape\"", "\"\\u12\"", "[1 2]"}) {
        EXPECT_FALSE(parse_json(bad)) << bad;
    }
}

TEST(Json, LimitsNesting) {
    std::string deep(40, '[');
    deep += std::string(40, ']');
    EXPECT_FALSE(parse_json(deep));
    std::string ok(20, '[');
    ok += std::string(20, ']');
    EXPECT_TRUE(parse_json(ok));
}

TEST(Status, ParsesFullStatus) {
    auto s = parse_status(kFull);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->device, "desk");
    EXPECT_EQ(s->id, "AAAA-BBBB");
    EXPECT_EQ(s->mountpoint, "/home/me/GhostDrive");
    EXPECT_EQ(s->port, 7373);
    ASSERT_TRUE(s->rendezvous);
    EXPECT_TRUE(s->rendezvous->connected);
    EXPECT_EQ(s->rendezvous->observed, "203.0.113.5:4000");
    EXPECT_FALSE(s->removed_by);
    ASSERT_EQ(s->peers.size(), 3u);
    EXPECT_EQ(peer_state(s->peers[0]), PeerState::relayed);
    EXPECT_EQ(peer_state(s->peers[1]), PeerState::offline);
    EXPECT_EQ(peer_state(s->peers[2]), PeerState::online);
    EXPECT_FALSE(s->peers[1].address);
    EXPECT_EQ(peer_state_label(PeerState::relayed), "Online through the relay");
    EXPECT_EQ(rendezvous_summary(*s), "Connected to rv.example:7374, seen as 203.0.113.5:4000");
}

TEST(Status, ParsesMinimalStatus) {
    auto s = parse_status(
        "{\"device\":\"laptop\",\"id\":\"abcd\",\"mountpoint\":null,\"port\":7373,"
        "\"rendezvous\":null,\"removed_by\":null,\"discovery_error\":null,\"peers\":[]}\n");
    ASSERT_TRUE(s);
    EXPECT_FALSE(s->mountpoint);
    EXPECT_FALSE(s->rendezvous);
    EXPECT_TRUE(s->peers.empty());
    EXPECT_EQ(rendezvous_summary(*s), "Not set up");
}

TEST(Status, CleansEveryString) {
    auto s = parse_status(
        "{\"device\":\"de\\u202esk\",\"id\":\"a\\nb\",\"mountpoint\":\"/m\\u0007\","
        "\"rendezvous\":{\"address\":\"rv\\u200b:1\",\"connected\":false,\"observed\":null},"
        "\"removed_by\":\"evil\\u2066\",\"discovery_error\":\"disk\\r\\nfull\","
        "\"peers\":[{\"name\":\"\\u001b[2Jphone\",\"id\":\"x\\u2029\",\"address\":\"\\u0000\","
        "\"online\":true,\"relayed\":false}]}");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->device, "de?sk");
    EXPECT_EQ(s->id, "a?b");
    EXPECT_EQ(s->mountpoint, "/m?");
    EXPECT_EQ(s->rendezvous->address, "rv?:1");
    EXPECT_EQ(rendezvous_summary(*s), "Not connected to rv?:1");
    EXPECT_EQ(s->removed_by, "evil?");
    EXPECT_EQ(s->discovery_error, "disk??full");
    EXPECT_EQ(s->peers[0].name, "?[2Jphone");
    EXPECT_EQ(s->peers[0].id, "x?");
    EXPECT_EQ(s->peers[0].address, "?");
}

TEST(Status, CutsLongNames) {
    std::string name(300, 'n');
    auto s = parse_status("{\"device\":\"" + name + "\",\"id\":\"i\",\"peers\":[]}");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->device.size(), 64u);
    EXPECT_EQ(s->device.substr(61), "...");
}

TEST(Status, RejectsOtherShapes) {
    EXPECT_FALSE(parse_status("[]"));
    EXPECT_FALSE(parse_status("{\"device\":\"d\",\"id\":\"i\"}"));
    EXPECT_FALSE(parse_status("{\"device\":1,\"id\":\"i\",\"peers\":[]}"));
    EXPECT_FALSE(parse_status("Device:   desk\n"));
    // A malformed peer is skipped, not fatal.
    auto s = parse_status("{\"device\":\"d\",\"id\":\"i\",\"peers\":[{\"name\":1},\"x\"]}");
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->peers.empty());
}

}  // namespace
}  // namespace ghostdrive::app
