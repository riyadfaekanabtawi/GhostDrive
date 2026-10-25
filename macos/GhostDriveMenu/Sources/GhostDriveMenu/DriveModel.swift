import AppKit
import Foundation

// Polls the daemon through the CLI and runs the menu's actions. It only ever sees what
// `ghostdrive` prints (status and invite codes); keys stay with the daemon.
@MainActor
final class DriveModel: ObservableObject {
    enum State: Equatable {
        case starting
        case running(DriveStatus)
        case notRunning(String)  // the CLI's explanation
        case noCLI
    }

    @Published private(set) var state: State = .starting
    @Published private(set) var message: String?  // result of the last action
    @Published private(set) var busy = false

    private var timer: Timer?
    private var polling = false
    private let queue = DispatchQueue(label: "ghostdrive.menu.cli")

    static let pollInterval: TimeInterval = 4

    init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: Self.pollInterval, repeats: true) {
            [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    var status: DriveStatus? {
        if case .running(let s) = state { return s }
        return nil
    }

    func refresh() {
        if polling { return }
        polling = true
        run(["status", "--json"]) { [weak self] result in
            guard let self else { return }
            self.polling = false
            self.state = Self.state(from: result)
        }
    }

    func copyInvite() {
        perform(["invite"]) { [weak self] output in
            guard let code = Self.inviteCode(in: output.stdout) else {
                return "Could not read an invite code from ghostdrive"
            }
            self?.copyToPasteboard(code)
            return "Invite code copied. It works once, for 10 minutes; send it privately."
        }
    }

    func openInFinder() {
        guard let mount = status?.mountpoint else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: mount))
    }

    func start() {
        perform(["autostart", "on"]) { _ in "GhostDrive is starting and will start at login" }
    }

    func stop() {
        perform(["stop"]) { _ in "GhostDrive stopped" }
    }

    func chooseCLI() {
        let panel = NSOpenPanel()
        panel.title = "Choose the ghostdrive command-line tool"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        NSApp.activate(ignoringOtherApps: true)
        if panel.runModal() == .OK, let url = panel.url {
            UserDefaults.standard.set(url.path, forKey: CLI.pathKey)
            refresh()
        }
    }

    func resetCLI() {
        UserDefaults.standard.removeObject(forKey: CLI.pathKey)
        refresh()
    }

    var customCLI: String? { UserDefaults.standard.string(forKey: CLI.pathKey) }

    // MARK: - Helpers

    private func perform(_ arguments: [String], onSuccess: @escaping (CLI.Output) -> String) {
        busy = true
        message = nil
        run(arguments) { [weak self] result in
            guard let self else { return }
            self.busy = false
            switch result {
            case .success(let output) where output.status == 0:
                self.message = onSuccess(output)
            case .success(let output):
                self.message = Self.firstLine(output.stderr.isEmpty ? output.stdout : output.stderr)
            case .failure(let failure):
                self.message = Self.describe(failure)
            }
            self.refresh()
        }
    }

    private func run(_ arguments: [String],
                     completion: @escaping @MainActor (Result<CLI.Output, CLI.Failure>) -> Void) {
        queue.async {
            let result = CLI.run(arguments)
            Task { @MainActor in completion(result) }
        }
    }

    private func copyToPasteboard(_ code: String) {
        let board = NSPasteboard.general
        board.clearContents()
        board.setString(code, forType: .string)
        // Asks clipboard managers that honour it not to keep a history of the code.
        board.setString("", forType: NSPasteboard.PasteboardType("org.nspasteboard.ConcealedType"))
    }

    private static func state(from result: Result<CLI.Output, CLI.Failure>) -> State {
        switch result {
        case .failure(.notFound):
            return .noCLI
        case .failure(let failure):
            return .notRunning(describe(failure))
        case .success(let output) where output.status == 0:
            guard let status = try? JSONDecoder().decode(DriveStatus.self,
                                                         from: Data(output.stdout.utf8)) else {
                return .notRunning("Unexpected status output; is ghostdrive up to date?")
            }
            return .running(status)
        case .success(let output):
            return .notRunning(firstLine(output.stderr))
        }
    }

    // `ghostdrive invite` prints "ghostdrive join gd1-..." among other lines.
    static func inviteCode(in text: String) -> String? {
        text.split(whereSeparator: { $0.isWhitespace })
            .first { $0.hasPrefix("gd1-") }
            .map(String.init)
    }

    private static func describe(_ failure: CLI.Failure) -> String {
        switch failure {
        case .notFound: return "ghostdrive command-line tool not found"
        case .launch(let reason): return "Could not run ghostdrive: \(reason)"
        case .timedOut: return "ghostdrive did not answer in time"
        }
    }

    private static func firstLine(_ text: String) -> String {
        var line = text.split(separator: "\n").first.map(String.init) ?? "ghostdrive failed"
        if line.hasPrefix("ghostdrive: ") { line.removeFirst("ghostdrive: ".count) }
        return line
    }
}
