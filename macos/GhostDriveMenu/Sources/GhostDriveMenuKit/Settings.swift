import Foundation

// The app's settings, kept in UserDefaults. An empty value means "use the default": the CLI's
// own default data directory and mountpoint, and the usual places to look for the CLI.
public struct MenuSettings: Equatable, Sendable {
    public static let cliPathKey = "ghostdriveCLIPath"
    public static let dataDirKey = "ghostdriveDataDir"
    public static let mountpointKey = "ghostdriveMountpoint"

    public var cliPath: String
    public var dataDir: String
    public var mountpoint: String

    public init(cliPath: String = "", dataDir: String = "", mountpoint: String = "") {
        self.cliPath = cliPath
        self.dataDir = dataDir
        self.mountpoint = mountpoint
    }

    public init(defaults: UserDefaults) {
        self.init(cliPath: defaults.string(forKey: Self.cliPathKey) ?? "",
                  dataDir: defaults.string(forKey: Self.dataDirKey) ?? "",
                  mountpoint: defaults.string(forKey: Self.mountpointKey) ?? "")
    }

    // The values as paths: trimmed, with a leading ~ expanded; nil when not set.
    public var cliURL: URL? { Self.path(cliPath).map { URL(fileURLWithPath: $0) } }
    public var dataPath: String? { Self.path(dataDir) }
    public var mountPath: String? { Self.path(mountpoint) }

    public static func path(_ value: String) -> String? {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return nil }
        return (trimmed as NSString).expandingTildeInPath
    }

    // Problems that keep the CLI from being run with these settings. The app starts the CLI
    // with "/" as its working directory, so relative paths would not mean what they say. The
    // mountpoint only matters to Start.
    public func problems(includingMount: Bool = true) -> [String] {
        var out: [String] = []
        if let cli = Self.path(cliPath), let problem = PathCheck.executable(cli) {
            out.append("ghostdrive tool: \(problem.description)")
        }
        if let data = dataPath, let problem = PathCheck.directory(data) {
            out.append("Data folder: \(problem.description)")
        }
        if includingMount, let mount = mountPath, let problem = PathCheck.directory(mount) {
            out.append("Mountpoint: \(problem.description)")
        }
        return out
    }
}

public enum PathProblem: Equatable, Sendable, CustomStringConvertible {
    case notAbsolute
    case missing
    case isDirectory
    case notDirectory
    case notExecutable

    public var description: String {
        switch self {
        case .notAbsolute: return "must be a full path starting with / or ~"
        case .missing: return "does not exist"
        case .isDirectory: return "is a folder, not a program"
        case .notDirectory: return "is a file, not a folder"
        case .notExecutable: return "is not executable"
        }
    }
}

public enum PathCheck {
    // The CLI must be an existing, executable file.
    public static func executable(_ path: String,
                                  fileManager: FileManager = .default) -> PathProblem? {
        guard path.hasPrefix("/") else { return .notAbsolute }
        var isDir: ObjCBool = false
        guard fileManager.fileExists(atPath: path, isDirectory: &isDir) else { return .missing }
        if isDir.boolValue { return .isDirectory }
        return fileManager.isExecutableFile(atPath: path) ? nil : .notExecutable
    }

    // The data folder and mountpoint need not exist yet (`ghostdrive init` and the daemon
    // create them), but must not be files.
    public static func directory(_ path: String,
                                 fileManager: FileManager = .default) -> PathProblem? {
        guard path.hasPrefix("/") else { return .notAbsolute }
        var isDir: ObjCBool = false
        if fileManager.fileExists(atPath: path, isDirectory: &isDir), !isDir.boolValue {
            return .notDirectory
        }
        return nil
    }

    // True when `path` is the root of a mounted file system, which is how the menu tells a
    // drive that is really mounted from a stale mountpoint in the status.
    public static func isMountPoint(_ path: String) -> Bool {
        guard let resolved = realpath(path, nil) else { return false }
        defer { free(resolved) }
        var fs = statfs()
        guard statfs(resolved, &fs) == 0 else { return false }
        let mountedOn = withUnsafeBytes(of: &fs.f_mntonname) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
        return mountedOn == String(cString: resolved)
    }
}
