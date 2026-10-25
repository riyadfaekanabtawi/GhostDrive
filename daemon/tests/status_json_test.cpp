#include "status_json.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::daemon {
namespace {

std::string quoted(std::string_view text) {
    std::string out;
    append_json_string(out, text);
    return out;
}

TEST(StatusJson, EscapesStrings) {
    EXPECT_EQ(quoted("plain"), "\"plain\"");
    EXPECT_EQ(quoted(""), "\"\"");
    EXPECT_EQ(quoted("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(quoted("\b\f\n\r\t"), "\"\\b\\f\\n\\r\\t\"");
    EXPECT_EQ(quoted(std::string_view("\x00\x01\x1f", 3)), "\"\\u0000\\u0001\\u001f\"");
    EXPECT_EQ(quoted("/slash and \x7f"), "\"/slash and \x7f\"");
    EXPECT_EQ(quoted("caf\xc3\xa9"), "\"caf\xc3\xa9\"");  // UTF-8 passes through
}

TEST(StatusJson, MinimalStatus) {
    ServiceStatus s;
    s.device_name = "laptop";
    s.fingerprint = "abcd";
    s.port = 7373;
    EXPECT_EQ(format_status_json(s, ""),
              "{\"device\":\"laptop\",\"id\":\"abcd\",\"mountpoint\":null,\"port\":7373,"
              "\"rendezvous\":null,\"removed_by\":null,\"discovery_error\":null,"
              "\"admission\":null,\"peers\":[]}\n");
}

TEST(StatusJson, FullStatus) {
    ServiceStatus s;
    s.device_name = "my \"laptop\"";
    s.fingerprint = "abcd";
    s.port = 1;
    s.rendezvous = "rv.example:7374";
    s.observed = asio::ip::udp::endpoint(asio::ip::make_address("203.0.113.5"), 4000);
    s.removed_by = "desk\ntop";
    s.discovery_error = "disk full";
    s.admission = "aa11";
    s.admission_generation = 3;
    s.earlier_admissions = {"bb22", "cc33"};
    s.members_caught_up = true;
    PeerStatus a;
    a.key.fill(std::byte{1});
    a.name = "phone";
    a.address = "192.0.2.1:7373";
    a.connected = true;
    a.relayed = true;
    PeerStatus b;
    b.name = "nas\\1";
    s.peers = {a, b};

    std::string expected =
        "{\"device\":\"my \\\"laptop\\\"\",\"id\":\"abcd\",\"mountpoint\":\"/Users/me/Ghost "
        "Drive\",\"port\":1,\"rendezvous\":{\"address\":\"rv.example:7374\",\"connected\":true,"
        "\"observed\":\"203.0.113.5:4000\"},\"removed_by\":\"desk\\ntop\","
        "\"discovery_error\":\"disk full\",\"admission\":{\"id\":\"aa11\",\"generation\":3,"
        "\"earlier\":[\"bb22\",\"cc33\"],\"members_caught_up\":true},\"peers\":["
        "{\"name\":\"phone\",\"id\":\"" +
        core::fingerprint(a.key) +
        "\",\"address\":\"192.0.2.1:7373\",\"online\":true,\"relayed\":true},"
        "{\"name\":\"nas\\\\1\",\"id\":\"" +
        core::fingerprint(b.key) + "\",\"address\":null,\"online\":false,\"relayed\":false}]}\n";
    EXPECT_EQ(format_status_json(s, "/Users/me/Ghost Drive"), expected);

    s.earlier_admissions.clear();
    s.members_caught_up = false;
    EXPECT_NE(format_status_json(s, "").find("\"admission\":{\"id\":\"aa11\",\"generation\":3,"
                                             "\"earlier\":[],\"members_caught_up\":false}"),
              std::string::npos);

    s.observed.reset();
    EXPECT_NE(format_status_json(s, "").find(
                  "\"rendezvous\":{\"address\":\"rv.example:7374\",\"connected\":false,"
                  "\"observed\":null}"),
              std::string::npos);
}

}  // namespace
}  // namespace ghostdrive::daemon
