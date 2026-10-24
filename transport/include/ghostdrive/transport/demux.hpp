#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace ghostdrive::transport {

// Prefix of GhostDrive's own non-QUIC datagrams (hole-punch probes, relay frames). The first
// byte 0x0D has the QUIC fixed bit (0x40) clear, so it never matches a QUIC packet while
// grease_quic_bit (RFC 9287) is not negotiated. It also falls in 4..15, which RFC 7983 and
// RFC 9443 leave unassigned, so it does not collide with STUN (0..3) or DTLS (20..63) either.
// The last prefix byte is the format version.
inline constexpr std::array<std::byte, 4> kDatagramMagic{std::byte{0x0D}, std::byte{'G'},
                                                         std::byte{'D'}, std::byte{0x01}};

enum class PacketKind {
    quic,
    ghostdrive,  // starts with kDatagramMagic
    unknown,
};

PacketKind classify_packet(std::span<const std::byte> packet) noexcept;

}  // namespace ghostdrive::transport
