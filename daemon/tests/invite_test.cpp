#include "ghostdrive/daemon/invite.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>

namespace ghostdrive::daemon {
namespace {

Invite sample() {
    Invite invite;
    invite.inviter.fill(std::byte{0xAB});
    for (std::size_t i = 0; i < invite.token.size(); ++i)
        invite.token[i] = static_cast<std::byte>(i);
    invite.addresses.emplace_back(asio::ip::make_address("192.168.1.20"), 7373);
    invite.addresses.emplace_back(asio::ip::make_address("fe80::1"), 7374);
    invite.addresses.emplace_back(asio::ip::make_address("127.0.0.1"), 7373);
    return invite;
}

TEST(Invite, RoundTrips) {
    auto code = encode_invite(sample());
    EXPECT_EQ(code.rfind("gd1-", 0), 0u);
    auto decoded = decode_invite(code);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value().inviter, sample().inviter);
    EXPECT_EQ(decoded.value().token, sample().token);
    EXPECT_EQ(decoded.value().addresses, sample().addresses);
}

TEST(Invite, AcceptsLowerCaseAndMissingDashes) {
    auto code = encode_invite(sample());
    std::string relaxed;
    for (char c : code) {
        if (c != '-') relaxed += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    auto decoded = decode_invite(relaxed);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().token, sample().token);
}

TEST(Invite, RejectsDamagedCodes) {
    auto code = encode_invite(sample());
    EXPECT_FALSE(decode_invite("hello"));
    EXPECT_FALSE(decode_invite("gd1-"));
    EXPECT_FALSE(decode_invite(code.substr(0, code.size() - 9)));  // truncated
    std::string bad = code;
    bad[bad.size() - 3] = '1';  // not in the base32 alphabet
    EXPECT_FALSE(decode_invite(bad));
    EXPECT_FALSE(decode_invite(code + "AAAAAAAA"));  // trailing data
}

}  // namespace
}  // namespace ghostdrive::daemon
