import Foundation

// Parsers for what the `ghostdrive` commands print (cli/ghostdrive.cpp and, for `remove`,
// Service::remove_device in daemon/src/service.cpp). Every string they return has been passed
// through DisplayText.clean.

// `ghostdrive init`:
//
//   Created a new drive on NAME.
//     device ID: XXXX-XXXX-...
//     data:      /path
public struct InitOutput: Equatable, Sendable {
    public var name: String
    public var id: String

    public static func parse(_ text: String) -> InitOutput? {
        var name: String?
        var id: String?
        for line in text.split(whereSeparator: \.isNewline) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("Created a new drive on "), trimmed.hasSuffix(".") {
                name = String(trimmed.dropFirst("Created a new drive on ".count).dropLast())
            } else if trimmed.hasPrefix("device ID:") {
                id = trimmed.dropFirst("device ID:".count).trimmingCharacters(in: .whitespaces)
            }
        }
        guard let name, let id, !id.isEmpty else { return nil }
        return InitOutput(name: DisplayText.clean(name), id: DisplayText.clean(id, maxLength: 128))
    }
}

// `ghostdrive join`:
//
//   Contacting the inviting device...
//   Joined the drive as NAME.
//   Start it with `ghostdrived`; files appear in ~/GhostDrive.
public enum JoinOutput {
    // The name this device joined as, or nil when the output does not say it joined.
    public static func joinedName(_ text: String) -> String? {
        for line in text.split(whereSeparator: \.isNewline) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("Joined the drive as "), trimmed.hasSuffix(".") {
                return DisplayText.clean(String(trimmed.dropFirst("Joined the drive as ".count)
                    .dropLast()))
            }
        }
        return nil
    }
}

// `ghostdrive remove`:
//
//   Removed NAME (ID). It can no longer connect to this drive's devices.
//   OTHER (ID2) was added by it. If you do not recognise it, remove it too: ghostdrive remove ID2
public struct RemoveOutput: Equatable, Sendable {
    public struct Device: Equatable, Sendable, Identifiable {
        public var name: String
        public var id: String

        public init(name: String, id: String) {
            self.name = name
            self.id = id
        }
    }

    public var removed: Device?
    // Devices that only the removed one vouched for; it may have made them up.
    public var addedByRemoved: [Device]
    // Every line, cleaned, to show as the command's text.
    public var lines: [String]

    public static func parse(_ text: String) -> RemoveOutput {
        var out = RemoveOutput(removed: nil, addedByRemoved: [], lines: [])
        let removedSuffix = "). It can no longer connect to this drive's devices."
        let hintMarker = ") was added by it."
        let hintCommand = "remove it too: ghostdrive remove "
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }
            out.lines.append(DisplayText.clean(line, maxLength: 400))
            if line.hasPrefix("Removed "), line.hasSuffix(removedSuffix) {
                let body = line.dropFirst("Removed ".count).dropLast(removedSuffix.count)
                if let open = body.range(of: " (", options: .backwards) {
                    out.removed = Device(name: DisplayText.clean(String(body[..<open.lowerBound])),
                                         id: DisplayText.clean(String(body[open.upperBound...]),
                                                               maxLength: 128))
                }
            } else if let marker = line.range(of: hintMarker),
                      let command = line.range(of: hintCommand, options: .backwards) {
                let id = String(line[command.upperBound...])
                let head = line[..<marker.lowerBound]
                guard head.hasSuffix(" (" + id), !id.isEmpty else { continue }
                let name = head.dropLast(id.count + 2)
                out.addedByRemoved.append(Device(name: DisplayText.clean(String(name)),
                                                 id: DisplayText.clean(id, maxLength: 128)))
            }
        }
        return out
    }
}

// A rendezvous server as `ghostdrive rendezvous` prints and takes it: "HOST:PORT FINGERPRINT",
// or "none" when there is none.
public struct RendezvousServer: Equatable, Sendable {
    public var address: String
    public var fingerprint: String

    public init(address: String, fingerprint: String) {
        self.address = address
        self.fingerprint = fingerprint
    }

    // The output of `ghostdrive rendezvous` with no arguments: .some(nil) for "none".
    public static func parseShown(_ text: String) -> RendezvousServer?? {
        let line = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if line == "none" { return .some(nil) }
        let parts = line.split(separator: " ", maxSplits: 1).map(String.init)
        let alphabet = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567-")
        guard parts.count == 2, parts[0].contains(":"), !parts[0].hasSuffix(":"),
              !parts[1].isEmpty, parts[1].allSatisfy({ alphabet.contains($0) })
        else { return nil }
        return .some(RendezvousServer(address: DisplayText.clean(parts[0], maxLength: 300),
                                      fingerprint: DisplayText.clean(parts[1], maxLength: 128)))
    }

    public enum InputError: Error, Equatable, Sendable, CustomStringConvertible {
        case missingAddress
        case badAddress
        case badFingerprint

        public var description: String {
            switch self {
            case .missingAddress: return "Enter the server's address as host:port"
            case .badAddress: return "The address must be host:port, with a port from 1 to 65535"
            case .badFingerprint:
                return "The fingerprint is the code the server printed when it started"
            }
        }
    }

    // What the user typed in Settings, checked the way RendezvousConfig::parse
    // (daemon/src/drive.cpp) will check it, so mistakes are explained before running the CLI.
    // Spaces and line breaks in a pasted fingerprint are dropped.
    public static func fromInput(address: String,
                                 fingerprint: String) -> Result<RendezvousServer, InputError> {
        let addr = address.trimmingCharacters(in: .whitespacesAndNewlines)
        if addr.isEmpty { return .failure(.missingAddress) }
        guard let colon = addr.lastIndex(of: ":"), colon != addr.startIndex,
              !addr.contains(where: { $0.isWhitespace }),
              let port = UInt16(addr[addr.index(after: colon)...]), port != 0
        else { return .failure(.badAddress) }
        let fp = fingerprint.filter { !$0.isWhitespace }
        let alphabet = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")
        let compact = fp.filter { $0 != "-" }
        // An Ed25519 key is 32 bytes, 52 base32 characters without padding.
        guard compact.count == 52, compact.allSatisfy({ alphabet.contains($0) })
        else { return .failure(.badFingerprint) }
        return .success(RendezvousServer(address: addr, fingerprint: fp))
    }
}

// `ghostdrive autostart status` on macOS:
//
//   login item: /Users/me/Library/LaunchAgents/org.ghostdrive....plist   (or "not set up")
//   launchd:    loaded                                                   (or "not loaded")
//   daemon:     /path/to/ghostdrived                                     (or "not found")
public struct AutostartState: Equatable, Sendable {
    public var enabled: Bool  // a login item exists
    public var loaded: Bool
    public var daemonFound: Bool

    public init(enabled: Bool, loaded: Bool, daemonFound: Bool) {
        self.enabled = enabled
        self.loaded = loaded
        self.daemonFound = daemonFound
    }

    public static func parse(_ text: String) -> AutostartState? {
        var fields: [String: String] = [:]
        for line in text.split(whereSeparator: \.isNewline) {
            guard let colon = line.firstIndex(of: ":") else { continue }
            fields[line[..<colon].trimmingCharacters(in: .whitespaces)] =
                line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
        }
        guard let item = fields["login item"] else { return nil }
        let loaded = fields["launchd"] ?? fields["systemd"] ?? ""
        return AutostartState(enabled: item != "not set up",
                              loaded: loaded == "loaded" || loaded == "active",
                              daemonFound: (fields["daemon"] ?? "not found") != "not found")
    }
}

extension DriveStatus.Peer {
    public enum State: String, Sendable {
        case online, relayed, offline
    }

    public var state: State {
        if !online { return .offline }
        return relayed ? .relayed : .online
    }
}

extension DriveStatus {
    // How the rendezvous server is doing, in words for the Home screen.
    public var rendezvousText: String {
        guard let rv = rendezvous else { return "Not set (local network only)" }
        if rv.connected {
            if let observed = rv.observed { return "Connected to \(rv.address), seen as \(observed)" }
            return "Connected to \(rv.address)"
        }
        return "Not connected to \(rv.address)"
    }
}
