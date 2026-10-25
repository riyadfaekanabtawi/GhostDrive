import Foundation

// The output of `ghostdrive status --json` (daemon/src/status_json.hpp).
struct DriveStatus: Decodable, Equatable {
    struct Rendezvous: Decodable, Equatable {
        var address: String
        var connected: Bool
        var observed: String?
    }

    struct Peer: Decodable, Equatable, Identifiable {
        var name: String
        var id: String
        var address: String?
        var online: Bool
        var relayed: Bool
    }

    var device: String
    var id: String
    var mountpoint: String?
    var port: Int
    var rendezvous: Rendezvous?
    var removedBy: String?
    var discoveryError: String?
    var peers: [Peer]

    enum CodingKeys: String, CodingKey {
        case device, id, mountpoint, port, rendezvous, peers
        case removedBy = "removed_by"
        case discoveryError = "discovery_error"
    }
}
