// LanDiscovery for builds without avahi-client or a usable DNS-SD library: start() always fails,
// so devices are reached only by stored addresses and the rendezvous server.
#include "ghostdrive/discovery/lan_discovery.hpp"

namespace ghostdrive::discovery {

struct LanDiscovery::Op {};

core::Result<std::unique_ptr<LanDiscovery>> LanDiscovery::start(asio::any_io_executor,
                                                                std::uint16_t, IdSource,
                                                                SightingHandler) {
    return core::Error{core::Errc::io, "built without LAN discovery"};
}

LanDiscovery::~LanDiscovery() = default;

void LanDiscovery::refresh() {}

}  // namespace ghostdrive::discovery
