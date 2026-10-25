import Foundation

// The argument lists the app passes to `ghostdrive` (cli/ghostdrive.cpp) and `ghostdrived`
// (daemon/ghostdrived.cpp), built from the settings in one place so the exact commands can be
// tested. Every `ghostdrive` command takes `--data`.
public enum Commands {
    public static let defaultInviteSeconds = 600

    public static func status(_ settings: AppSettings) -> [String] {
        withData(["status", "--json"], settings)
    }

    public static func invite(_ settings: AppSettings,
                              seconds: Int = defaultInviteSeconds) -> [String] {
        withData(["invite", "--ttl", String(seconds)], settings)
    }

    public static func stop(_ settings: AppSettings) -> [String] {
        withData(["stop"], settings)
    }

    public static func id(_ settings: AppSettings) -> [String] {
        withData(["id"], settings)
    }

    public static func initDrive(_ settings: AppSettings, name: String) -> [String] {
        withData(["init", "--name", name], settings)
    }

    // The port goes to the inviting device as this device's listening port, so it must match
    // the one Start passes to ghostdrived.
    public static func join(_ settings: AppSettings, code: String, name: String) -> [String] {
        var args = ["join", code, "--name", name]
        if let port = settings.portNumber { args += ["--port", String(port)] }
        return withData(args, settings)
    }

    // By ID: names need not be unique, and an ID cannot be mistaken for an option.
    public static func remove(_ settings: AppSettings, id: String) -> [String] {
        withData(["remove", id], settings)
    }

    public static func rendezvous(_ settings: AppSettings) -> [String] {
        withData(["rendezvous"], settings)
    }

    public static func setRendezvous(_ settings: AppSettings,
                                     _ server: RendezvousServer) -> [String] {
        withData(["rendezvous", server.address, server.fingerprint], settings)
    }

    // Sets up the login item and starts the daemon now. Without a data folder or mountpoint
    // setting the CLI uses its own defaults.
    public static func autostartOn(_ settings: AppSettings) -> [String] {
        var args = withData(["autostart", "on"], settings)
        if let mount = settings.mountPath { args += ["--mount", mount] }
        return args
    }

    // Removes the login item; the CLI also stops the daemon.
    public static func autostartOff(_ settings: AppSettings) -> [String] {
        withData(["autostart", "off"], settings)
    }

    public static func autostartStatus(_ settings: AppSettings) -> [String] {
        withData(["autostart", "status"], settings)
    }

    // The arguments for running `ghostdrived` directly (Start, without a login item).
    public static func daemon(_ settings: AppSettings) -> [String] {
        var args = ["--data", settings.effectiveDataDir, "--mount", settings.effectiveMountpoint]
        if let port = settings.portNumber { args += ["--port", String(port)] }
        return args
    }

    static func withData(_ args: [String], _ settings: AppSettings) -> [String] {
        guard let data = settings.dataPath else { return args }
        return args + ["--data", data]
    }
}

// What `ghostdrive invite` prints (cli/ghostdrive.cpp, cmd_invite):
//
//   On the other device, run:
//
//     ghostdrive join gd1-XXXXXXXX-XXXXXXXX-...
//   The code works once, for 10 minutes. Anyone who has it can join, so send it privately.
public struct Invite: Equatable, Sendable {
    public var code: String
    public var minutes: Int?  // nil when the output does not say

    public init(code: String, minutes: Int?) {
        self.code = code
        self.minutes = minutes
    }

    public static func parse(_ text: String) -> Invite? {
        guard let code = text.split(whereSeparator: { $0.isWhitespace })
            .map(String.init).first(where: isCode) else { return nil }
        return Invite(code: code, minutes: minutes(in: text))
    }

    // "gd1-" and dash-separated groups of base32 (core/src/bytes.cpp, daemon/src/invite.cpp).
    public static func isCode(_ word: String) -> Bool {
        guard word.hasPrefix("gd1-") else { return false }
        let groups = word.dropFirst(4).split(separator: "-", omittingEmptySubsequences: false)
        let alphabet = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")
        return !groups.isEmpty && groups.allSatisfy { group in
            !group.isEmpty && group.allSatisfy { alphabet.contains($0) }
        }
    }

    // The code in what the user typed or pasted: the bare code, or a whole
    // `ghostdrive join gd1-...` line. Nil when there is none.
    public static func code(inInput text: String) -> String? {
        text.split(whereSeparator: { $0.isWhitespace }).map(String.init).first(where: isCode)
    }

    static func minutes(in text: String) -> Int? {
        guard let range = text.range(of: #"for (\d+) minutes?"#, options: .regularExpression)
        else { return nil }
        return Int(text[range].split(separator: " ")[1])
    }

    public var confirmation: String {
        guard let minutes else { return "Invite copied; it works once" }
        return "Invite copied, valid \(minutes) min; it works once"
    }

    // Seconds left of an invite created at `created` for `seconds`, never below zero.
    public static func remaining(created: Date, seconds: Int, now: Date) -> Int {
        max(0, Int((created.timeIntervalSince(now) + Double(seconds)).rounded(.up)))
    }

    // "9:05" for 545 seconds.
    public static func countdown(_ seconds: Int) -> String {
        let s = max(0, seconds)
        return "\(s / 60):" + String(format: "%02d", s % 60)
    }
}

// Text from the daemon or the CLI made safe to show. The daemon already replaces control and
// bidirectional characters in peer names, but this device's own name and other strings are
// passed through, and the app should not trust either. Control, format (which includes every
// bidirectional mark, embedding, override and isolate, and zero-width characters) and line or
// paragraph separators become "?"; long text is cut short.
public enum DisplayText {
    public static let maxLength = 64

    public static func clean(_ text: String, maxLength: Int = maxLength) -> String {
        var out = String.UnicodeScalarView()
        for scalar in text.unicodeScalars {
            switch scalar.properties.generalCategory {
            case .control, .format, .lineSeparator, .paragraphSeparator, .surrogate,
                 .privateUse:
                out.append("?")
            default:
                out.append(scalar)
            }
        }
        let cleaned = String(out)
        guard cleaned.count > maxLength else { return cleaned }
        return String(cleaned.prefix(maxLength - 3)) + "..."
    }

    // Up to `maxLines` non-empty lines of CLI output, each cleaned, with the "ghostdrive: "
    // prefix the CLI puts on errors removed.
    public static func lines(_ text: String, maxLines: Int = 4,
                             maxLength: Int = 120) -> [String] {
        let lines = text.split(whereSeparator: \.isNewline)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
            .map { line -> String in
                line.hasPrefix("ghostdrive: ") ? String(line.dropFirst("ghostdrive: ".count))
                                               : line
            }
        return lines.prefix(maxLines).map { clean($0, maxLength: maxLength) }
    }

    // The CLI's messages name terminal commands; in the app those are buttons and screens.
    public static func appWording(_ line: String) -> String {
        line.replacingOccurrences(of: "Start it with `ghostdrived`.", with: "Start GhostDrive first.")
            .replacingOccurrences(of: "ask for a new one with `ghostdrive invite`",
                                  with: "ask for a new one under Add device on the other device")
    }

    // The lines for a command that exited with a nonzero status: the CLI's error output (or
    // its normal output when it wrote no errors), after an optional heading.
    public static func failure(_ heading: String?, status: Int32, stdout: String,
                               stderr: String, maxLines: Int = 4,
                               maxLength: Int = 120) -> [String] {
        var out = lines(stderr.isEmpty ? stdout : stderr, maxLines: maxLines,
                        maxLength: maxLength)
        if out.isEmpty { out = ["ghostdrive exited with status \(status)"] }
        if let heading { out.insert(heading + ":", at: 0) }
        return out
    }
}

extension DriveStatus {
    // A copy with every string the app shows passed through DisplayText.clean, and an empty
    // mountpoint treated as none.
    public func cleaned() -> DriveStatus {
        var s = self
        s.device = DisplayText.clean(device)
        s.id = DisplayText.clean(id, maxLength: 128)
        if let m = mountpoint, m.isEmpty { s.mountpoint = nil }
        s.mountpoint = s.mountpoint.map { DisplayText.clean($0, maxLength: 1024) }
        s.removedBy = removedBy.map { DisplayText.clean($0) }
        s.discoveryError = discoveryError.map { DisplayText.clean($0, maxLength: 120) }
        if var rv = rendezvous {
            rv.address = DisplayText.clean(rv.address)
            rv.observed = rv.observed.map { DisplayText.clean($0) }
            s.rendezvous = rv
        }
        s.peers = peers.map { peer in
            var p = peer
            p.name = DisplayText.clean(peer.name)
            p.id = DisplayText.clean(peer.id, maxLength: 128)
            p.address = peer.address.map { DisplayText.clean($0) }
            return p
        }
        return s
    }
}
