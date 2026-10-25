#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ghostdrive::app {

// The output of `ghostdrive status --json` (daemon/src/status_json.hpp), with every string
// already passed through clean_text.
struct Peer {
    std::string name;
    std::string id;
    std::optional<std::string> address;
    bool online = false;
    bool relayed = false;

    bool operator==(const Peer&) const = default;
};

struct RendezvousStatus {
    std::string address;
    bool connected = false;
    std::optional<std::string> observed;

    bool operator==(const RendezvousStatus&) const = default;
};

struct DriveStatus {
    std::string device;
    std::string id;
    std::optional<std::string> mountpoint;  // nullopt when not mounted
    long port = 0;
    std::optional<RendezvousStatus> rendezvous;  // nullopt when no server is set
    std::optional<std::string> removed_by;
    std::optional<std::string> discovery_error;
    std::vector<Peer> peers;

    bool operator==(const DriveStatus&) const = default;
};

// Returns nullopt unless the text is a JSON object with at least "device", "id" and "peers".
std::optional<DriveStatus> parse_status(std::string_view json);

enum class PeerState { online, relayed, offline };

PeerState peer_state(const Peer& peer);

// "Online", "Online through the relay" or "Offline".
std::string_view peer_state_label(PeerState state);

// One line about the rendezvous server for the home page, for example
// "Connected to rv.example:7374, seen as 203.0.113.5:4433" or "Not set up".
std::string rendezvous_summary(const DriveStatus& status);

}  // namespace ghostdrive::app
