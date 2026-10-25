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
// groups, keeping its connection and so its relayed connections. The device asks for peers with
// Connect; the server answers PeerAt to both sides, so both can start hole punching at once.
namespace ghostdrive::discovery {

inline constexpr std::size_t kGroupIdSize = 32;
using GroupId = std::array<std::byte, kGroupIdSize>;

// HMAC-SHA256(key, "ghostdrive-rendezvous-group-v1"), where the key is the drive's current
// discovery key. Only members can compute it, and it reveals nothing about the key to the
// server. Fails only if OpenSSL cannot compute it.
core::Result<GroupId> rendezvous_group(const core::DriveSecret& key);

// HMAC-SHA256(key, "ghostdrive-rendezvous-admission-v1") of a discovery key of generation 1 or
// higher: what a server's --allow-group list holds for a drive. It differs from the group, so
// an operator who knows it cannot join the drive's group. For generation 0 (the drive secret)
// the admission ID is rendezvous_group(drive_secret) instead, the value allow lists held before
// admission IDs rotated. Fails only if OpenSSL cannot compute it.
core::Result<GroupId> rendezvous_admission(const core::DriveSecret& key);

struct Register {
    GroupId group{};
    // What a server's --allow-group list is checked against; any one listed admits the client.
    // Devices send the current key's ID, then the earlier keys' IDs, then the generation-0 ID,
    // so lists from before a change of key keep working. A Register from an older client has
    // one: the drive secret's group, or for a 32-byte Register, `group`.
    std::vector<GroupId> admissions;
    // Groups of earlier discovery keys, where members that have not caught up still are. The
    // client may Connect to devices in them, but nobody finds the client there.
    std::vector<GroupId> hidden;
};
inline constexpr std::size_t kMaxAdmissions = 5;
inline constexpr std::size_t kMaxHiddenGroups = 4;

struct Registered {
    asio::ip::udp::endpoint observed;
};
struct Connect {
    core::PublicKey peer{};
};
struct PeerAt {
    core::PublicKey peer{};
    std::optional<asio::ip::udp::endpoint> address;  // nullopt: not online in this group
};

// group | count u8 | admissions | count u8 | hidden groups. At most kMaxAdmissions and
// kMaxHiddenGroups are sent.
core::Bytes encode(const Register& m);
// The form servers from before rotating admission IDs take: group | admission | count u8 |
// hidden groups, with the last of `admissions` (the generation-0 ID).
core::Bytes encode_fixed_register(const Register& m);
core::Bytes encode(const Registered& m);
core::Bytes encode(const Connect& m);
core::Bytes encode(const PeerAt& m);
// Takes all three forms; their lengths differ modulo 32 (0, 1 and 2).
core::Result<Register> decode_register(std::span<const std::byte> payload);
core::Result<Registered> decode_registered(std::span<const std::byte> payload);
core::Result<Connect> decode_connect(std::span<const std::byte> payload);
core::Result<PeerAt> decode_peer_at(std::span<const std::byte> payload);

}  // namespace ghostdrive::discovery
