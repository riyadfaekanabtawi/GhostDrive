#pragma once

#include <array>
#include <asio/ip/udp.hpp>
#include <cstddef>
#include <optional>
#include <span>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"

// The rendezvous control stream, between a device and its rendezvous server (ARCHITECTURE.md
// section 3.5). The device opens one stream and sends Register; the server answers Registered
// with the address it sees. The device asks for peers with Connect; the server answers PeerAt
// to both sides, so both can start hole punching at once.
namespace ghostdrive::discovery {

inline constexpr std::size_t kGroupIdSize = 32;
using GroupId = std::array<std::byte, kGroupIdSize>;

// HMAC-SHA256(drive_secret, "ghostdrive-rendezvous-group-v1"). Only members can compute it, and
// it reveals nothing about the secret to the server. Fails only if OpenSSL cannot compute it.
core::Result<GroupId> rendezvous_group(const core::DriveSecret& secret);

struct Register {
    GroupId group{};
};
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
