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

std::vector<std::byte> make_probe() {
    std::vector<std::byte> out(kDatagramMagic.begin(), kDatagramMagic.end());
    out.push_back(kDatagramProbe);
    return out;
}

}  // namespace ghostdrive::transport
