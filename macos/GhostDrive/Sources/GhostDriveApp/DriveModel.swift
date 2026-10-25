import AppKit
import Foundation
import GhostDriveKit

// Polls the daemon through the CLI and runs the app's actions. It only ever sees what
// `ghostdrive` prints (status, IDs and invite codes); keys stay with the CLI and the daemon.
@MainActor
final class DriveModel: ObservableObject {
    enum State: Equatable {
        case starting
        case noCLI
        case badCLI(String)  // the tool set in Settings cannot be run
        case noDrive  // the data folder holds no drive yet: first run
        case notRunning(String)  // the CLI's explanation
        // `mountpoint` is the raw path, for opening; the status holds a cleaned copy to show.
        case running(DriveStatus, mountpoint: String?, mounted: Bool)
    }

    // The actions whose progress and result are shown, each in its own place.
    enum Action: Hashable {
        case setup, startStop, invite, menu, remove, rendezvous, autostart
    }

    struct Feedback: Equatable {
        var ok: Bool
        var lines: [String]
    }

    struct ActiveInvite: Equatable {
        var code: String
        var created: Date
        var seconds: Int
        var knownPeers: Set<String>
    }

    @Published private(set) var state: State = .starting
    @Published private(set) var busy: Set<Action> = []
    @Published private(set) var feedback: [Action: Feedback] = [:]
    @Published private(set) var deviceID: String?  // from `ghostdrive id` while not running
    @Published private(set) var invite: ActiveInvite?
    @Published private(set) var joinedSinceInvite: [String] = []  // names, cleaned
    @Published private(set) var removal: RemoveOutput?
    @Published private(set) var rendezvousSetting: RendezvousServer??  // nil: not read yet
    @Published private(set) var autostart: AutostartState?
    @Published private(set) var settings = AppSettings(defaults: .standard)

    private var timer: Timer?
    private var polling = false
    private var defaultsObserver: NSObjectProtocol?
    private let queue = DispatchQueue(label: "ghostdrive.app.cli", attributes: .concurrent)

    static let pollInterval: TimeInterval = 3

    init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: Self.pollInterval, repeats: true) {
            [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        // Settings changes take effect at once.
        defaultsObserver = NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification, object: nil, queue: .main) {
            [weak self] _ in
            Task { @MainActor in self?.settingsChanged() }
        }
    }

    var status: DriveStatus? {
        if case .running(let s, _, _) = state { return s }
        return nil
    }

    // The mountpoint only while it is really mounted.
    var mountedAt: String? {
        if case .running(_, let m, true) = state { return m }
        return nil
    }

    var isBusy: Bool { !busy.isEmpty }

    func isBusy(_ action: Action) -> Bool { busy.contains(action) }

    func clearFeedback(_ action: Action) { feedback[action] = nil }

    private func settingsChanged() {
        let current = AppSettings(defaults: .standard)
        if current == settings { return }
        settings = current
        feedback = [:]
        deviceID = nil
        invite = nil
        removal = nil
        rendezvousSetting = nil
        autostart = nil
        state = .starting
        refresh()
    }

    func refresh() {
        if polling { return }
        polling = true
        let settings = self.settings
        let needID = deviceID == nil
        queue.async {
            let state = Self.poll(settings)
            var id: String?
            if needID, case .notRunning = state,
               case .success(let out) = CLI.run(Commands.id(settings), settings: settings),
               out.status == 0 {
                id = DisplayText.clean(out.stdout.trimmingCharacters(in: .whitespacesAndNewlines),
                                       maxLength: 128)
            }
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.polling = false
                guard settings == self.settings else { return self.refresh() }
                self.state = state
                if let id { self.deviceID = id }
                if case .running(let s, _, _) = state { self.deviceID = s.id }
                self.noticeJoins()
            }
        }
    }

    // MARK: - First run

    func create(name: String) {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            feedback[.setup] = Feedback(ok: false, lines: ["Give this device a name first."])
            return
        }
        perform(.setup, Commands.initDrive(settings, name: name), heading: "Could not create the drive",
                progress: ["Creating the drive..."]) { [weak self] output in
            self?.deviceID = InitOutput.parse(output.stdout)?.id
            self?.start(after: .setup)
            return ["Drive created. Starting GhostDrive..."]
        }
    }

    func join(code input: String, name: String) {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            feedback[.setup] = Feedback(ok: false, lines: ["Give this device a name first."])
            return
        }
        guard let code = Invite.code(inInput: input) else {
            feedback[.setup] = Feedback(ok: false, lines: [
                "That is not an invite code. It starts with gd1- and comes from Add device on a "
                    + "device that is already in the drive."])
            return
        }
        perform(.setup, Commands.join(settings, code: code, name: name),
                heading: "Could not join the drive", timeout: 90,
                progress: ["Contacting the inviting device..."], maxLines: 6) { [weak self] output in
            self?.start(after: .setup)
            let joined = JoinOutput.joinedName(output.stdout).map { "Joined the drive as \($0)." }
            return [joined ?? "Joined the drive.", "Starting GhostDrive..."]
        }
    }

    // MARK: - Start and stop

    // Starts ghostdrived directly. This creates no login item; that is "Start at login".
    func start(after action: Action = .startStop) {
        let problems = settings.problems(includingMount: true)
        if !problems.isEmpty {
            feedback[action] = Feedback(ok: false, lines: ["Fix the settings first:"]
                + problems.map { DisplayText.clean($0, maxLength: 120) })
            return
        }
        busy.insert(action)
        feedback[action] = Feedback(ok: true, lines: (feedback[action]?.lines ?? [])
            .filter { !$0.hasPrefix("Starting") } + ["Starting GhostDrive..."])
        let settings = self.settings
        queue.async {
            let result = Self.startAndWait(settings)
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.busy.remove(action)
                switch result {
                case .success:
                    self.feedback[action] = Feedback(ok: true, lines: ["GhostDrive is running."])
                case .failure(let failure):
                    self.feedback[action] = Feedback(ok: false,
                                                     lines: ["GhostDrive did not start:"] + failure.lines)
                }
                self.refresh()
            }
        }
    }

    func stop() {
        perform(.startStop, Commands.stop(settings), heading: "Could not stop GhostDrive",
                progress: ["Stopping GhostDrive..."]) { _ in ["GhostDrive stopped."] }
    }

    // MARK: - Invites

    func createInvite(copy: Bool = false) {
        let seconds = Commands.defaultInviteSeconds
        let known = Set(status?.peers.map(\.id) ?? [])
        let action: Action = copy ? .menu : .invite
        perform(action, Commands.invite(settings, seconds: seconds),
                heading: "Could not create an invite",
                progress: ["Creating an invite..."]) { [weak self] output in
            guard let parsed = Invite.parse(output.stdout) else {
                return ["Could not read an invite code from ghostdrive."]
            }
            self?.invite = ActiveInvite(code: parsed.code, created: Date(), seconds: seconds,
                                        knownPeers: known)
            self?.joinedSinceInvite = []
            if copy {
                self?.copyToPasteboard(parsed.code)
                return [parsed.confirmation]
            }
            return []
        }
    }

    func copyInviteCode() {
        guard let invite else { return }
        copyToPasteboard(invite.code)
        feedback[.invite] = Feedback(ok: true, lines: ["Copied."])
    }

    func discardInvite() {
        invite = nil
        joinedSinceInvite = []
    }

    // Names devices that joined while an invite was shown, so the screen can say it was used.
    private func noticeJoins() {
        guard let invite, let status else { return }
        let names = status.peers.filter { !invite.knownPeers.contains($0.id) }.map(\.name)
        if names != joinedSinceInvite { joinedSinceInvite = names }
    }

    // MARK: - Devices

    func remove(id: String) {
        removal = nil
        perform(.remove, Commands.remove(settings, id: id), heading: "Could not remove the device",
                progress: ["Removing the device..."], maxLength: 400) { [weak self] output in
            let parsed = RemoveOutput.parse(output.stdout)
            self?.removal = parsed
            return parsed.lines
        }
    }

    // MARK: - Settings

    func loadRendezvous() {
        let settings = self.settings
        queue.async {
            let result = CLI.run(Commands.rendezvous(settings), settings: settings)
            Task { @MainActor [weak self] in
                guard let self, settings == self.settings else { return }
                if case .success(let out) = result, out.status == 0,
                   let shown = RendezvousServer.parseShown(out.stdout) {
                    self.rendezvousSetting = .some(shown)
                }
            }
        }
    }

    func setRendezvous(address: String, fingerprint: String) {
        switch RendezvousServer.fromInput(address: address, fingerprint: fingerprint) {
        case .failure(let error):
            feedback[.rendezvous] = Feedback(ok: false, lines: [error.description])
        case .success(let server):
            perform(.rendezvous, Commands.setRendezvous(settings, server),
                    heading: "Could not set the rendezvous server",
                    progress: ["Saving..."], maxLines: 6) { [weak self] output in
                self?.loadRendezvous()
                return DisplayText.lines(output.stdout, maxLines: 4, maxLength: 200)
            }
        }
    }

    func loadAutostart() {
        let settings = self.settings
        queue.async {
            let result = CLI.run(Commands.autostartStatus(settings), settings: settings)
            Task { @MainActor [weak self] in
                guard let self, settings == self.settings else { return }
                if case .success(let out) = result, out.status == 0 {
                    self.autostart = AutostartState.parse(out.stdout)
                }
            }
        }
    }

    // On: creates a login item and starts GhostDrive. Off: removes it and stops GhostDrive.
    func setAutostart(_ on: Bool) {
        let args = on ? Commands.autostartOn(settings) : Commands.autostartOff(settings)
        perform(.autostart, args,
                heading: on ? "Could not turn on Start at login" : "Could not turn off Start at login",
                timeout: 60, progress: ["Working..."], needsMount: on, maxLength: 200) {
            [weak self] output in
            self?.loadAutostart()
            return DisplayText.lines(output.stdout, maxLines: 2, maxLength: 200)
        }
    }

    func openDrive() {
        guard let mount = mountedAt else { return }
        // Off the main thread: opening a folder on the FUSE-T mount can block in LaunchServices
        // for a long time, and the window must stay responsive.
        DispatchQueue.global(qos: .userInitiated).async {
            NSWorkspace.shared.open(URL(fileURLWithPath: mount))
        }
    }

    // MARK: - Helpers

    private func perform(_ action: Action, _ arguments: [String], heading: String,
                         timeout: TimeInterval = 15, progress: [String] = [],
                         needsMount: Bool = false, maxLines: Int = 4, maxLength: Int = 160,
                         onSuccess: @escaping (CLI.Output) -> [String]) {
        let problems = settings.problems(includingMount: needsMount)
        if !problems.isEmpty {
            feedback[action] = Feedback(ok: false, lines: ["Fix the settings first:"]
                + problems.map { DisplayText.clean($0, maxLength: 120) })
            return
        }
        busy.insert(action)
        feedback[action] = progress.isEmpty ? nil : Feedback(ok: true, lines: progress)
        let settings = self.settings
        queue.async {
            let result = CLI.run(arguments, settings: settings, timeout: timeout)
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.busy.remove(action)
                switch result {
                case .success(let output) where output.status == 0:
                    let lines = onSuccess(output)
                    // When onSuccess started a follow-up (Start), that reports for itself unless
                    // there is something to say first.
                    if !lines.isEmpty || !self.busy.contains(action) {
                        self.feedback[action] = lines.isEmpty ? nil : Feedback(ok: true, lines: lines)
                    }
                case .success(let output):
                    self.feedback[action] = Feedback(
                        ok: false,
                        lines: DisplayText.failure(heading, status: output.status,
                                                   stdout: output.stdout, stderr: output.stderr,
                                                   maxLines: maxLines, maxLength: maxLength)
                            .map(Self.plain))
                case .failure(let failure):
                    self.feedback[action] = Feedback(ok: false,
                                                     lines: [heading + ":", CLI.describe(failure)])
                }
                self.refresh()
            }
        }
    }

    nonisolated static func plain(_ line: String) -> String { DisplayText.appWording(line) }

    private func copyToPasteboard(_ code: String) {
        let board = NSPasteboard.general
        board.clearContents()
        board.setString(code, forType: .string)
        // Asks clipboard managers that honour it not to keep a history of the code.
        board.setString("", forType: NSPasteboard.PasteboardType("org.nspasteboard.ConcealedType"))
    }

    // Runs on the CLI queue.
    nonisolated private static func poll(_ settings: AppSettings) -> State {
        switch CLI.locate(settings) {
        case .failure(.notFound): return .noCLI
        case .failure(.badPath(let path, let problem)):
            return .badCLI("\(DisplayText.clean(path, maxLength: 120)) \(problem.description)")
        case .failure(let failure): return .notRunning(CLI.describe(failure))
        case .success: break
        }
        if !PathCheck.hasDrive(dataDir: settings.effectiveDataDir) { return .noDrive }
        return state(from: CLI.run(Commands.status(settings), settings: settings))
    }

    // Starts the daemon and waits until it answers, or reports why it did not.
    nonisolated private static func startAndWait(_ settings: AppSettings) -> Result<Void, Lines> {
        let pid: pid_t
        switch CLI.startDaemon(settings: settings) {
        case .success(let p): pid = p
        case .failure(let failure): return .failure(Lines([CLI.describe(failure)]))
        }
        for _ in 0..<100 {
            if case .success(let out) = CLI.run(Commands.status(settings), settings: settings),
               out.status == 0 {
                return .success(())
            }
            if kill(pid, 0) != 0 { break }  // it exited
            usleep(150_000)
        }
        // The end of the daemon's log says why; the daemon never logs keys.
        let log = (settings.effectiveDataDir as NSString).appendingPathComponent("ghostdrived.log")
        let text = (try? String(contentsOfFile: log, encoding: .utf8)) ?? ""
        let tail = DisplayText.lines(text, maxLines: 200, maxLength: 200).suffix(3)
        return .failure(Lines(tail.isEmpty ? ["It did not answer in time."] : Array(tail)))
    }

    struct Lines: Error {
        var lines: [String]
        init(_ lines: [String]) { self.lines = lines }
    }

    // Runs on the CLI queue: the mount check can block on a stuck file system.
    nonisolated private static func state(from result: Result<CLI.Output, CLI.Failure>) -> State {
        switch result {
        case .failure(let failure):
            return .notRunning(CLI.describe(failure))
        case .success(let output) where output.status == 0:
            guard let decoded = try? JSONDecoder().decode(DriveStatus.self,
                                                          from: Data(output.stdout.utf8)) else {
                return .notRunning("Unexpected status output; is ghostdrive up to date?")
            }
            let raw = decoded.mountpoint.flatMap { $0.isEmpty ? nil : $0 }
            let mounted = raw.map { PathCheck.isMountPoint($0) } ?? false
            return .running(decoded.cleaned(), mountpoint: raw, mounted: mounted)
        case .success(let output):
            return .notRunning(plain(DisplayText.lines(output.stderr, maxLines: 1).first
                                     ?? "ghostdrive failed"))
        }
    }
}
