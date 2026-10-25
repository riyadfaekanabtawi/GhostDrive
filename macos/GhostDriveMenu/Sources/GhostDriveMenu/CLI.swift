import Foundation

// Runs the `ghostdrive` command-line tool. The menu talks to the daemon only through it, so the
// control-socket location and the data directory rules live in one place (the CLI).
struct CLI {
    static let pathKey = "ghostdriveCLIPath"  // user setting: full path to the CLI
    static let dataKey = "ghostdriveDataDir"  // user setting: passed as --data when set

    struct Output {
        var status: Int32
        var stdout: String
        var stderr: String
    }

    enum Failure: Error {
        case notFound
        case launch(String)
        case timedOut
    }

    // The configured path, then `ghostdrive` inside or next to the app bundle, then PATH and
    // the usual install locations (apps started from Finder get a minimal PATH).
    static func locate() -> URL? {
        let fm = FileManager.default
        if let configured = UserDefaults.standard.string(forKey: pathKey), !configured.isEmpty {
            let url = URL(fileURLWithPath: (configured as NSString).expandingTildeInPath)
            return fm.isExecutableFile(atPath: url.path) ? url : nil
        }
        var candidates: [URL] = []
        if let exe = Bundle.main.executableURL?.deletingLastPathComponent() {
            candidates.append(exe.appendingPathComponent("ghostdrive"))
        }
        candidates.append(Bundle.main.bundleURL.deletingLastPathComponent()
            .appendingPathComponent("ghostdrive"))
        let path = ProcessInfo.processInfo.environment["PATH"] ?? ""
        let dirs = path.split(separator: ":").map(String.init)
            + ["/opt/homebrew/bin", "/usr/local/bin", NSHomeDirectory() + "/.local/bin"]
        for dir in dirs where !dir.isEmpty {
            candidates.append(URL(fileURLWithPath: dir).appendingPathComponent("ghostdrive"))
        }
        return candidates.first { fm.isExecutableFile(atPath: $0.path) }
    }

    // Runs `ghostdrive <arguments> [--data DIR]` without a shell. Blocks; call off the main
    // thread.
    static func run(_ arguments: [String], timeout: TimeInterval = 15) -> Result<Output, Failure> {
        guard let tool = locate() else { return .failure(.notFound) }
        var args = arguments
        if let data = UserDefaults.standard.string(forKey: dataKey), !data.isEmpty {
            args += ["--data", (data as NSString).expandingTildeInPath]
        }

        let process = Process()
        process.executableURL = tool
        process.arguments = args
        let out = Pipe()
        let err = Pipe()
        process.standardOutput = out
        process.standardError = err
        process.standardInput = FileHandle.nullDevice
        do {
            try process.run()
        } catch {
            return .failure(.launch(error.localizedDescription))
        }

        let timer = DispatchWorkItem { if process.isRunning { process.terminate() } }
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout, execute: timer)
        // Read stderr concurrently so neither pipe can fill up and stall the child.
        var errData = Data()
        let errDone = DispatchSemaphore(value: 0)
        DispatchQueue.global().async {
            errData = err.fileHandleForReading.readDataToEndOfFile()
            errDone.signal()
        }
        let outData = out.fileHandleForReading.readDataToEndOfFile()
        errDone.wait()
        process.waitUntilExit()
        timer.cancel()
        if process.terminationReason == .uncaughtSignal { return .failure(.timedOut) }
        return .success(Output(status: process.terminationStatus,
                               stdout: String(decoding: outData, as: UTF8.self),
                               stderr: String(decoding: errData, as: UTF8.self)))
    }
}
