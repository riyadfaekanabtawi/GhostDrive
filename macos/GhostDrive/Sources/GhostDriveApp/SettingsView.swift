import AppKit
import GhostDriveKit
import SwiftUI

// The Settings screen. Paths are stored in UserDefaults as they are typed and the model picks
// them up through UserDefaults.didChangeNotification; empty means the default. The rendezvous
// server and Start at login are set through the CLI.
struct SettingsView: View {
    @ObservedObject var model: DriveModel
    @AppStorage(AppSettings.dataDirKey) private var dataDir = ""
    @AppStorage(AppSettings.cliPathKey) private var cliPath = ""
    @AppStorage(AppSettings.mountpointKey) private var mountpoint = ""
    @State private var rvAddress = ""
    @State private var rvFingerprint = ""

    private var hasDrive: Bool {
        switch model.state {
        case .running, .notRunning: return true
        default: return false
        }
    }

    var body: some View {
        Form {
            Section {} header: {
                ScreenTitle(title: "Settings").foregroundStyle(.primary)
            }
            Group {
                Section("GhostDrive") {
                    HStack {
                        StatusDot(color: model.status == nil ? .secondary : .green)
                        Text(model.status == nil ? "Not running" : "Running")
                        Spacer()
                        if model.status != nil {
                            Button("Stop GhostDrive") { model.stop() }
                                .disabled(model.isBusy(.startStop))
                        } else {
                            Button("Start GhostDrive") { model.start() }
                                .disabled(model.isBusy(.startStop) || !hasDrive)
                        }
                    }
                    FeedbackView(feedback: model.feedback[.startStop],
                                 busy: model.isBusy(.startStop))
                    Toggle("Start at login", isOn: Binding(
                        get: { model.autostart?.enabled ?? false },
                        set: { model.setAutostart($0) }))
                        .disabled(model.autostart == nil || model.isBusy(.autostart) || !hasDrive)
                    Text("Adds a login item that starts GhostDrive now and at every login. "
                         + "Turning it off also stops GhostDrive.")
                        .font(.caption).foregroundStyle(.secondary)
                    FeedbackView(feedback: model.feedback[.autostart],
                                 busy: model.isBusy(.autostart))
                }

                Section("Rendezvous server") {
                    LabeledContent("Current") {
                        Text(rendezvousText).textSelection(.enabled)
                    }
                    TextField("Address", text: $rvAddress, prompt: Text("host:port"))
                    LabeledContent("Fingerprint") {
                        TextField("Fingerprint", text: $rvFingerprint,
                                  prompt: Text("printed by ghostdrive-rendezvous"))
                            .labelsHidden()
                            .font(.system(.body, design: .monospaced))
                    }
                    HStack {
                        Text("For reaching devices on other networks. Devices that join later "
                             + "get it automatically.")
                            .font(.caption).foregroundStyle(.secondary)
                        Spacer()
                        Button("Set Server") {
                            model.setRendezvous(address: rvAddress, fingerprint: rvFingerprint)
                        }
                        .disabled(model.isBusy(.rendezvous) || !hasDrive
                                  || rvAddress.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                    FeedbackView(feedback: model.feedback[.rendezvous],
                                 busy: model.isBusy(.rendezvous))
                }

                Section("Folders") {
                    PathRow(title: "Data folder", value: $dataDir,
                            placeholder: AppSettings.defaultDataDir(), chooseDirectory: true,
                            problem: AppSettings.path(dataDir).flatMap { PathCheck.directory($0) })
                    Text("Where this device's keys, index and file pieces live (--data).")
                        .font(.caption).foregroundStyle(.secondary)
                    PathRow(title: "Mount location", value: $mountpoint,
                            placeholder: AppSettings.defaultMountpoint(), chooseDirectory: true,
                            problem: AppSettings.path(mountpoint).flatMap { PathCheck.directory($0) })
                    Text("Where the drive appears (--mount). Takes effect the next time it starts.")
                        .font(.caption).foregroundStyle(.secondary)
                    PathRow(title: "ghostdrive tool", value: $cliPath,
                            placeholder: "inside or next to the app, or on PATH",
                            chooseDirectory: false,
                            problem: AppSettings.path(cliPath).flatMap { PathCheck.executable($0) })
                    Text("The command-line tool the app runs for every action.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .onAppear {
            model.loadRendezvous()
            model.loadAutostart()
        }
    }

    private var rendezvousText: String {
        switch model.rendezvousSetting {
        case .none: return hasDrive ? "..." : "-"
        case .some(.none): return "None (local network only)"
        case .some(.some(let server)): return "\(server.address)  \(server.fingerprint)"
        }
    }
}

private struct PathRow: View {
    let title: String
    @Binding var value: String
    let placeholder: String
    let chooseDirectory: Bool
    let problem: PathProblem?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                TextField(title, text: $value, prompt: Text(placeholder))
                Button("Choose...") { choose() }
                Button("Default") { value = "" }
                    .disabled(value.isEmpty)
            }
            if let problem {
                Text("\(title) \(problem.description)")
                    .font(.caption).foregroundColor(.red)
            }
        }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.title = chooseDirectory ? "Choose a folder for \(title.lowercased())"
                                      : "Choose the ghostdrive command-line tool"
        panel.canChooseFiles = !chooseDirectory
        panel.canChooseDirectories = chooseDirectory
        panel.canCreateDirectories = chooseDirectory
        panel.allowsMultipleSelection = false
        panel.treatsFilePackagesAsDirectories = true
        if let current = AppSettings.path(value) {
            panel.directoryURL = URL(fileURLWithPath: current)
        }
        if panel.runModal() == .OK, let url = panel.url {
            value = url.path
        }
    }
}
