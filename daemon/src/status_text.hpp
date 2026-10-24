#pragma once

#include <sstream>
#include <string>

#include "ghostdrive/daemon/service.hpp"

namespace ghostdrive::daemon {

// The `ghostdrive status` text.
inline std::string format_status(const ServiceStatus& status, const std::string& mountpoint) {
    std::ostringstream out;
    out << "Device:    " << status.device_name << "\n";
    out << "ID:        " << status.fingerprint << "\n";
    out << "Mounted:   " << (mountpoint.empty() ? "no" : mountpoint) << "\n";
    out << "Listening: UDP port " << status.port << "\n";
    if (status.peers.empty()) {
        out << "Devices:   none yet (add one with `ghostdrive invite`)\n";
        return out.str();
    }
    out << "Devices:\n";
    for (const auto& p : status.peers) {
        out << "  " << (p.connected ? "online   " : "offline  ") << p.name;
        if (!p.address.empty()) out << "  (" << p.address << ")";
        out << "\n";
    }
    return out.str();
}

}  // namespace ghostdrive::daemon
