#include "ghostdrive/app/status.hpp"

#include "ghostdrive/app/json.hpp"
#include "ghostdrive/app/text.hpp"

namespace ghostdrive::app {

namespace {

constexpr std::size_t kNameChars = 64;
constexpr std::size_t kIdChars = 128;
constexpr std::size_t kLongChars = 200;

using Kind = JsonValue::Kind;

std::optional<std::string> string_field(const JsonValue& object, std::string_view key,
                                        std::size_t max_chars) {
    const JsonValue* v = object.find(key);
    if (!v || !v->is(Kind::string) || v->string.empty()) return std::nullopt;
    return clean_text(v->string, max_chars);
}

bool bool_field(const JsonValue& object, std::string_view key) {
    const JsonValue* v = object.find(key);
    return v && v->is(Kind::boolean) && v->boolean;
}

}  // namespace

std::optional<DriveStatus> parse_status(std::string_view json) {
    auto root = parse_json(json);
    if (!root || !root->is(Kind::object)) return std::nullopt;
    const JsonValue* device = root->find("device");
    const JsonValue* id = root->find("id");
    const JsonValue* peers = root->find("peers");
    if (!device || !device->is(Kind::string) || !id || !id->is(Kind::string) || !peers ||
        !peers->is(Kind::array)) {
        return std::nullopt;
    }

    DriveStatus out;
    out.device = clean_text(device->string, kNameChars);
    out.id = clean_text(id->string, kIdChars);
    out.mountpoint = string_field(*root, "mountpoint", kLongChars);
    if (const JsonValue* port = root->find("port"); port && port->is(Kind::number)) {
        out.port = static_cast<long>(port->integer);
    }
    if (const JsonValue* rv = root->find("rendezvous"); rv && rv->is(Kind::object)) {
        RendezvousStatus r;
        r.address = string_field(*rv, "address", kNameChars).value_or("");
        r.connected = bool_field(*rv, "connected");
        r.observed = string_field(*rv, "observed", kNameChars);
        out.rendezvous = std::move(r);
    }
    out.removed_by = string_field(*root, "removed_by", kNameChars);
    out.discovery_error = string_field(*root, "discovery_error", kLongChars);
    for (const JsonValue& p : peers->items) {
        if (!p.is(Kind::object)) continue;
        const JsonValue* name = p.find("name");
        const JsonValue* pid = p.find("id");
        if (!name || !name->is(Kind::string) || !pid || !pid->is(Kind::string)) continue;
        Peer peer;
        peer.name = clean_text(name->string, kNameChars);
        peer.id = clean_text(pid->string, kIdChars);
        peer.address = string_field(p, "address", kNameChars);
        peer.online = bool_field(p, "online");
        peer.relayed = bool_field(p, "relayed");
        out.peers.push_back(std::move(peer));
    }
    return out;
}

PeerState peer_state(const Peer& peer) {
    if (!peer.online) return PeerState::offline;
    return peer.relayed ? PeerState::relayed : PeerState::online;
}

std::string_view peer_state_label(PeerState state) {
    switch (state) {
        case PeerState::online:
            return "Online";
        case PeerState::relayed:
            return "Online through the relay";
        case PeerState::offline:
            break;
    }
    return "Offline";
}

std::string rendezvous_summary(const DriveStatus& status) {
    if (!status.rendezvous) return "Not set up";
    const auto& rv = *status.rendezvous;
    if (!rv.connected) return "Not connected to " + rv.address;
    std::string out = "Connected to " + rv.address;
    if (rv.observed) out += ", seen as " + *rv.observed;
    return out;
}

}  // namespace ghostdrive::app
