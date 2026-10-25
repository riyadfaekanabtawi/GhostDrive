import Foundation
import GhostDriveMenuKit

// Runs the `ghostdrive` command-line tool. The menu talks to the daemon only through it, so the
// control-socket location and the data directory rules live in one place (the CLI).
struct CLI {
    struct Output {
        var status: Int32
        var stdout: String
        var stderr: String
    }

    enum Failure: Error {
        case notFound
        case badPath(String, PathProblem)  // the path set in Settings is not usable
        case launch(String)
        case timedOut
    }

    // The configured path, then `ghostdrive` inside or next to the app bundle, then PATH and
    // the usual install locations (apps started from Finder get a minimal PATH).
    static func locate(_ settings: MenuSettings) -> Result<URL, Failure> {
        let fm = FileManager.default
        if let configured = settings.cliURL {
            if let problem = PathCheck.executable(configured.path) {
                return .failure(.badPath(configured.path, problem))
            }
            return .success(configured)
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
        let found = candidates.first { fm.isExecutableFile(atPath: $0.path) }
        return found.map { .success($0) } ?? .failure(.notFound)
    }

    // Runs `ghostdrive <arguments>` (see Commands) without a shell. Blocks; call off the main
    // thread.
    static func run(_ arguments: [String], settings: MenuSettings,
                    timeout: TimeInterval = 15) -> Result<Output, Failure> {
        let tool: URL
        switch locate(settings) {
        case .success(let url): tool = url
        case .failure(let failure): return .failure(failure)
        }

        let process = Process()
        process.executableURL = tool
        process.arguments = arguments
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
