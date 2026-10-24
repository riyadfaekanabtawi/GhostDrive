#pragma once

#include <asio/ip/udp.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/daemon/codec.hpp"

namespace ghostdrive::daemon {

// What a new device needs to join: the inviter's full public key (pinned when dialing, so no
// man in the middle can stand in for it), a one-time token proving the holder was invited, and
// where to reach the inviter. This is the text form of the QR code in ARCHITECTURE.md
// section 2.3; whoever reads the code can join until it expires, so share it privately.
struct Invite {
    core::PublicKey inviter{};
    Token token{};
    std::vector<asio::ip::udp::endpoint> addresses;
};

// "gd1-" followed by RFC 4648 base32 (no padding), in groups of eight for reading aloud.
std::string encode_invite(const Invite& invite);
// Accepts the code with or without the dashes, in any case.
core::Result<Invite> decode_invite(std::string_view code);

}  // namespace ghostdrive::daemon
