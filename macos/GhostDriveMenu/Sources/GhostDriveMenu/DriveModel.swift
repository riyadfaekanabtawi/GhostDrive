import AppKit
import Foundation
import GhostDriveMenuKit

// Polls the daemon through the CLI and runs the menu's actions. It only ever sees what
// `ghostdrive` prints (status and invite codes); keys stay with the daemon.
@MainActor
final class DriveModel: ObservableObject {
    enum State: Equatable {
        case starting
        case running(DriveStatus, mounted: Bool)
        case notRunning(String)  // the CLI's explanation
        case noCLI
        case badCLI(String)  // the tool set in Settings cannot be run
    }

    @Published private(set) var state: State = .starting
    @Published private(set) var message: [String] = []  // result of the last action
    @Published private(set) var busy = false

    private var timer: Timer?
    private var polling = false
    private var defaultsObserver: NSObjectProtocol?
    private var settings = MenuSettings(defaults: .standard)
    private let queue = DispatchQueue(label: "ghostdrive.menu.cli")

    static let pollInterval: TimeInterval = 4

    init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: Self.pollInterval, repeats: true) {
            [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        // Settings changes take effect at the next poll; poll now so the menu follows at once.
        defaultsObserver = NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification, object: nil, queue: .main) {
            [weak self] _ in
            Task { @MainActor in self?.settingsChanged() }
        }
    }

    var status: DriveStatus? {
        if case .running(let s, _) = state { return s }
        return nil
    }

    // The mountpoint only while it is really mounted.
    var mountedAt: String? {
        if case .running(let s, true) = state { return s.mountpoint }
        return nil
    }

    private func settingsChanged() {
        let current = MenuSettings(defaults: .standard)
        if current == settings { return }
        settings = current
        message = []
        refresh()
    }

    func refresh() {
        if polling { return }
        polling = true
        let settings = self.settings
        queue.async {
            let result = CLI.run(Commands.status(settings), settings: settings)
            let state = Self.state(from: result)
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.polling = false
                self.state = state
            }
        }
    }

    func copyInvite() {
        perform(Commands.invite(settings)) { [weak self] output in
            guard let invite = Invite.parse(output.stdout) else {
                return ["Could not read an invite code from ghostdrive"]
            }
            self?.copyToPasteboard(invite.code)
            return [invite.confirmation]
        }
    }

    func openInFinder() {
        guard let mount = mountedAt else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: mount))
    }

    // Runs `ghostdrive autostart on` with the data folder and mountpoint from Settings. This
    // creates a login item, so it only ever runs when the user clicks Start.
    func start() {
        perform(Commands.start(settings), failurePrefix: "Start failed", timeout: 60,
                needsMount: true) { _ in
            ["GhostDrive is starting and will start at login"]
        }
    }

    func stop() {
        perform(Commands.stop(settings), failurePrefix: "Stop failed") { _ in
            ["GhostDrive stopped"]
        }
    }

    // macOS 13 only; later versions open Settings through SettingsLink.
    func openSettings() {
        NSApp.activate(ignoringOtherApps: true)
        NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
    }

    // MARK: - Helpers

    private func perform(_ arguments: [String], failurePrefix: String? = nil,
                         timeout: TimeInterval = 15, needsMount: Bool = false,
                         onSuccess: @escaping (CLI.Output) -> [String]) {
        let problems = settings.problems(includingMount: needsMount)
        if !problems.isEmpty {
            message = ["Fix the settings first:"]
                + problems.map { DisplayText.clean($0, maxLength: 120) }
            return
        }
        busy = true
        message = []
        let settings = self.settings
        queue.async {
            let result = CLI.run(arguments, settings: settings, timeout: timeout)
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.busy = false
                switch result {
                case .success(let output) where output.status == 0:
                    self.message = onSuccess(output)
                case .success(let output):
                    self.message = DisplayText.failure(failurePrefix, status: output.status,
                                                       stdout: output.stdout,
                                                       stderr: output.stderr)
                case .failure(let failure):
                    self.message = [Self.describe(failure)]
                }
                self.refresh()
            }
        }
    }

    private func copyToPasteboard(_ code: String) {
        let board = NSPasteboard.general
        board.clearContents()
        board.setString(code, forType: .string)
        // Asks clipboard managers that honour it not to keep a history of the code.
        board.setString("", forType: NSPasteboard.PasteboardType("org.nspasteboard.ConcealedType"))
    }

    // Runs on the CLI queue: the mount check can block on a stuck file system.
    nonisolated private static func state(from result: Result<CLI.Output, CLI.Failure>) -> State {
        switch result {
        case .failure(.notFound):
            return .noCLI
        case .failure(.badPath(let path, let problem)):
            return .badCLI("\(DisplayText.clean(path, maxLength: 120)) \(problem.description)")
        case .failure(let failure):
            return .notRunning(describe(failure))
        case .success(let output) where output.status == 0:
            guard let decoded = try? JSONDecoder().decode(DriveStatus.self,
                                                          from: Data(output.stdout.utf8)) else {
                return .notRunning("Unexpected status output; is ghostdrive up to date?")
            }
            let status = decoded.cleaned()
            let mounted = decoded.mountpoint.map { !$0.isEmpty && PathCheck.isMountPoint($0) }
            return .running(status, mounted: mounted ?? false)
        case .success(let output):
            return .notRunning(DisplayText.lines(output.stderr, maxLines: 1).first
                               ?? "ghostdrive failed")
        }
    }

    nonisolated private static func describe(_ failure: CLI.Failure) -> String {
        switch failure {
        case .notFound: return "ghostdrive command-line tool not found"
        case .badPath(let path, let problem):
            return "\(DisplayText.clean(path, maxLength: 120)) \(problem.description)"
        case .launch(let reason): return "Could not run ghostdrive: \(DisplayText.clean(reason))"
        case .timedOut: return "ghostdrive did not answer in time"
        }
    }
}
