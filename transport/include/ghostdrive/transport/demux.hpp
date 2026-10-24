#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ghostdrive::transport {

// Prefix of GhostDrive's own non-QUIC datagrams (hole-punch probes, relay frames). The first
// byte 0x0D has the QUIC fixed bit (0x40) clear, so it never matches a QUIC packet while
// grease_quic_bit (RFC 9287) is not negotiated. It also falls in 4..15, which RFC 7983 and
// RFC 9443 leave unassigned, so it does not collide with STUN (0..3) or DTLS (20..63) either.
// The last prefix byte is the format version.
inline constexpr std::array<std::byte, 4> kDatagramMagic{std::byte{0x0D}, std::byte{'G'},
                                                         std::byte{'D'}, std::byte{0x01}};

// Datagram types after kDatagramMagic:
//   probe: nothing more. Sent to open NAT mappings while hole punching; otherwise ignored.
//   relay: a 16-byte registration token (the client's own, in both directions), a 32-byte key
//          (client to server: the destination; server to client: the source), then one QUIC
//          packet, end-to-end encrypted, which the server never reads.
inline constexpr std::byte kDatagramProbe{'P'};
inline constexpr std::byte kDatagramRelay{'R'};
inline constexpr std::size_t kRelayTokenBytes = 16;
inline constexpr std::size_t kRelayHeaderSize = kDatagramMagic.size() + 1 + kRelayTokenBytes + 32;

enum class PacketKind {
    quic,
    ghostdrive,  // starts with kDatagramMagic
    unknown,
};

PacketKind classify_packet(std::span<const std::byte> packet) noexcept;

struct RelayDatagram {
    std::span<const std::byte, kRelayTokenBytes> token;
    std::span<const std::byte, 32> key;
    std::span<const std::byte> payload;
};
// Splits a relay datagram; nullopt if it is anything else or too short.
std::optional<RelayDatagram> parse_relay(std::span<const std::byte> datagram) noexcept;
// COPY: builds a relay datagram around `payload` (one QUIC packet).
std::vector<std::byte> make_relay(std::span<const std::byte, kRelayTokenBytes> token,
                                  std::span<const std::byte, 32> key,
                                  std::span<const std::byte> payload);
std::vector<std::byte> make_probe();

}  // namespace ghostdrive::transport
