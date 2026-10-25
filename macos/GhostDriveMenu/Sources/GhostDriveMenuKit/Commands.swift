import Foundation

// The argument lists the menu passes to `ghostdrive`, built from the settings in one place so
// the exact commands can be tested.
public enum Commands {
    public static func status(_ settings: MenuSettings) -> [String] {
        withData(["status", "--json"], settings)
    }

    public static func invite(_ settings: MenuSettings) -> [String] {
        withData(["invite"], settings)
    }

    public static func stop(_ settings: MenuSettings) -> [String] {
        withData(["stop"], settings)
    }

    // Sets up the login item and starts the daemon now. Without a data folder or mountpoint
    // setting the CLI uses its own defaults.
    public static func start(_ settings: MenuSettings) -> [String] {
        var args = withData(["autostart", "on"], settings)
        if let mount = settings.mountPath { args += ["--mount", mount] }
        return args
    }

    static func withData(_ args: [String], _ settings: MenuSettings) -> [String] {
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
public struct Invite: Equatable {
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
    static func isCode(_ word: String) -> Bool {
        guard word.hasPrefix("gd1-") else { return false }
        let groups = word.dropFirst(4).split(separator: "-", omittingEmptySubsequences: false)
        let alphabet = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")
        return !groups.isEmpty && groups.allSatisfy { group in
            !group.isEmpty && group.allSatisfy { alphabet.contains($0) }
        }
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
}

// Text from the daemon or the CLI made safe to show in the menu. The daemon already replaces
// control and bidirectional characters in peer names, but this device's own name and other
// strings are passed through, and the menu should not trust either. Control, format (which
// includes every bidirectional mark, embedding, override and isolate, and zero-width
// characters) and line or paragraph separators become "?"; long text is cut short.
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

    // The menu lines for a command that exited with a nonzero status: the CLI's error output
    // (or its normal output when it wrote no errors), after an optional heading.
    public static func failure(_ heading: String?, status: Int32, stdout: String,
                               stderr: String) -> [String] {
        var out = lines(stderr.isEmpty ? stdout : stderr)
        if out.isEmpty { out = ["ghostdrive exited with status \(status)"] }
        if let heading { out.insert(heading + ":", at: 0) }
        return out
    }
}

extension DriveStatus {
    // A copy with every string the menu shows passed through DisplayText.clean, and an empty
    // mountpoint treated as none.
    public func cleaned() -> DriveStatus {
        var s = self
        s.device = DisplayText.clean(device)
        s.id = DisplayText.clean(id, maxLength: 128)
        if let m = mountpoint, m.isEmpty { s.mountpoint = nil }
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
