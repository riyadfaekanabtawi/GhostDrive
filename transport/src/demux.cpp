#include "ghostdrive/transport/demux.hpp"

#include <algorithm>

namespace ghostdrive::transport {

PacketKind classify_packet(std::span<const std::byte> packet) noexcept {
    if (packet.empty()) return PacketKind::unknown;
    if ((std::to_integer<unsigned>(packet[0]) & 0x40u) != 0) return PacketKind::quic;
    if (packet.size() >= kDatagramMagic.size() &&
        std::equal(kDatagramMagic.begin(), kDatagramMagic.end(), packet.begin())) {
        return PacketKind::ghostdrive;
    }
    return PacketKind::unknown;
}

std::optional<RelayDatagram> parse_relay(std::span<const std::byte> datagram) noexcept {
    if (datagram.size() < kRelayHeaderSize || classify_packet(datagram) != PacketKind::ghostdrive ||
        datagram[kDatagramMagic.size()] != kDatagramRelay) {
        return std::nullopt;
    }
    auto rest = datagram.subspan(kDatagramMagic.size() + 1);
    return RelayDatagram{rest.first<kRelayTokenBytes>(),
                         rest.subspan(kRelayTokenBytes).first<32>(),
                         rest.subspan(kRelayTokenBytes + 32)};
}

std::vector<std::byte> make_relay(std::span<const std::byte, kRelayTokenBytes> token,
                                  std::span<const std::byte, 32> key,
                                  std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    out.reserve(kRelayHeaderSize + payload.size());
    out.insert(out.end(), kDatagramMagic.begin(), kDatagramMagic.end());
    out.push_back(kDatagramRelay);
    out.insert(out.end(), token.begin(), token.end());
    out.insert(out.end(), key.begin(), key.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::byte> make_probe() {
    std::vector<std::byte> out(kDatagramMagic.begin(), kDatagramMagic.end());
    out.push_back(kDatagramProbe);
    return out;
}

}  // namespace ghostdrive::transport
