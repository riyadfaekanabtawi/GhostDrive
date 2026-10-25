#pragma once

#include <array>
#include <asio/ip/udp.hpp>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"

// The rendezvous control stream, between a device and its rendezvous server (ARCHITECTURE.md
// section 3.5). The device opens one stream and sends Register; the server answers Registered
// with the address it sees. A later Register on the same stream moves the device to other
// groups, keeping its relay token. The device asks for peers with Connect; the server answers
// PeerAt to both sides, so both can start hole punching at once.
namespace ghostdrive::discovery {

inline constexpr std::size_t kGroupIdSize = 32;
using GroupId = std::array<std::byte, kGroupIdSize>;

// HMAC-SHA256(key, "ghostdrive-rendezvous-group-v1"), where the key is the drive's current
// discovery key. Only members can compute it, and it reveals nothing about the key to the
// server. Fails only if OpenSSL cannot compute it.
core::Result<GroupId> rendezvous_group(const core::DriveSecret& key);

struct Register {
    GroupId group{};
    // What a server's --allow-group list is checked against: the group derived from the drive
    // secret, which stays the same when the discovery key is replaced. A Register without it
    // (from an older client) uses `group`.
    GroupId admission{};
    // Groups of earlier discovery keys, where members that have not caught up still are. The
    // client may Connect to devices in them, but nobody finds the client there.
    std::vector<GroupId> hidden;
};
inline constexpr std::size_t kMaxHiddenGroups = 4;
inline constexpr std::size_t kRelayTokenSize = 16;
using RelayToken = std::array<std::byte, kRelayTokenSize>;

struct Registered {
    asio::ip::udp::endpoint observed;
    // Random per registration; carried in every relay datagram so the server knows the sender
    // without trusting the (spoofable) source address, and the client knows the server sent it.
    RelayToken relay_token{};
};
struct Connect {
    core::PublicKey peer{};
};
struct PeerAt {
    core::PublicKey peer{};
    std::optional<asio::ip::udp::endpoint> address;  // nullopt: not online in this group
};

core::Bytes encode(const Register& m);
core::Bytes encode(const Registered& m);
core::Bytes encode(const Connect& m);
core::Bytes encode(const PeerAt& m);
core::Result<Register> decode_register(std::span<const std::byte> payload);
core::Result<Registered> decode_registered(std::span<const std::byte> payload);
core::Result<Connect> decode_connect(std::span<const std::byte> payload);
core::Result<PeerAt> decode_peer_at(std::span<const std::byte> payload);

}  // namespace ghostdrive::discovery
