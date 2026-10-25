import Foundation

// The output of `ghostdrive status --json` (daemon/src/status_json.hpp).
public struct DriveStatus: Decodable, Equatable, Sendable {
    public struct Rendezvous: Decodable, Equatable, Sendable {
        public var address: String
        public var connected: Bool
        public var observed: String?
    }

    public struct Peer: Decodable, Equatable, Sendable, Identifiable {
        public var name: String
        public var id: String
        public var address: String?
        public var online: Bool
        public var relayed: Bool
    }

    public var device: String
    public var id: String
    public var mountpoint: String?
    public var port: Int
    public var rendezvous: Rendezvous?
    public var removedBy: String?
    public var discoveryError: String?
    public var peers: [Peer]

    enum CodingKeys: String, CodingKey {
        case device, id, mountpoint, port, rendezvous, peers
        case removedBy = "removed_by"
        case discoveryError = "discovery_error"
    }
}
