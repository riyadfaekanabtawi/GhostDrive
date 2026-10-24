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
    if (!status.removed_by.empty()) {
        out << "Removed:   " << status.removed_by
            << " removed this device from the drive; the other devices no longer accept it\n";
    }
    if (!status.rendezvous.empty()) {
        out << "Rendezvous: " << status.rendezvous;
        if (status.observed) {
            out << " (connected; seen as " << status.observed->address().to_string() << ":"
                << status.observed->port() << ")";
        } else {
            out << " (not connected)";
        }
        out << "\n";
    }
    if (!status.rendezvous_group.empty()) out << "Group:     " << status.rendezvous_group << "\n";
    if (status.peers.empty()) {
        out << "Devices:   none yet (add one with `ghostdrive invite`)\n";
        return out.str();
    }
    out << "Devices:\n";
    for (const auto& p : status.peers) {
        out << "  " << (p.connected ? (p.relayed ? "relayed  " : "online   ") : "offline  ") << p.name;
        if (!p.address.empty()) out << "  (" << p.address << ")";
        out << "\n           ID " << core::fingerprint(p.key);
        out << "\n";
    }
    return out.str();
}

}  // namespace ghostdrive::daemon
