import Darwin
import Foundation
import GhostDriveKit

// Runs the `ghostdrive` command-line tool. The app talks to the daemon only through it, so the
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
        case noDaemon(String)  // no ghostdrived next to this ghostdrive
    }

    // The configured path, then `ghostdrive` inside or next to the app bundle, then PATH and
    // the usual install locations (apps started from Finder get a minimal PATH).
    static func locate(_ settings: AppSettings) -> Result<URL, Failure> {
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
    static func run(_ arguments: [String], settings: AppSettings,
                    timeout: TimeInterval = 15) -> Result<Output, Failure> {
        let tool: URL
        switch locate(settings) {
        case .success(let url): tool = url
        case .failure(let failure): return .failure(failure)
        }

        let process = Process()
        process.executableURL = tool
        process.arguments = arguments
        process.currentDirectoryURL = URL(fileURLWithPath: "/")
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

    // Starts `ghostdrived` (the one next to the ghostdrive tool) in its own session, so it
    // keeps running when the app quits, with its output appended to ghostdrived.log in the data
    // folder as the login item does. Returns the daemon's process ID.
    static func startDaemon(settings: AppSettings) -> Result<pid_t, Failure> {
        let tool: URL
        switch locate(settings) {
        case .success(let url): tool = url
        case .failure(let failure): return .failure(failure)
        }
        guard let daemon = PathCheck.daemon(nextTo: tool) else {
            return .failure(.noDaemon(tool.deletingLastPathComponent().path))
        }
        let log = (settings.effectiveDataDir as NSString).appendingPathComponent("ghostdrived.log")
        let argv = [daemon.path] + Commands.daemon(settings)

        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log,
                                         O_WRONLY | O_CREAT | O_APPEND, 0o600)
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO)
        posix_spawn_file_actions_addchdir_np(&actions, "/")
        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETSID))

        var cargs = argv.map { strdup($0) } + [nil]
        defer { cargs.forEach { free($0) } }
        var pid: pid_t = 0
        let rc = posix_spawn(&pid, daemon.path, &actions, &attr, &cargs, environ)
        if rc != 0 { return .failure(.launch(String(cString: strerror(rc)))) }
        // Reap it when it exits, so a daemon that stops while the app runs leaves no zombie.
        DispatchQueue.global().async {
            var status: Int32 = 0
            while waitpid(pid, &status, 0) < 0 && errno == EINTR {}
        }
        return .success(pid)
    }

    static func describe(_ failure: Failure) -> String {
        switch failure {
        case .notFound: return "The ghostdrive command-line tool was not found"
        case .badPath(let path, let problem):
            return "\(DisplayText.clean(path, maxLength: 120)) \(problem.description)"
        case .launch(let reason): return "Could not run ghostdrive: \(DisplayText.clean(reason))"
        case .timedOut: return "ghostdrive did not answer in time"
        case .noDaemon(let dir):
            return "ghostdrived was not found next to ghostdrive in "
                + DisplayText.clean(dir, maxLength: 120)
        }
    }
}
