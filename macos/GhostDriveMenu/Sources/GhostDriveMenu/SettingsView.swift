import AppKit
import GhostDriveMenuKit
import SwiftUI

// The Settings window. Values are stored in UserDefaults as they are typed; the menu picks
// them up through UserDefaults.didChangeNotification. Empty means the default.
struct SettingsView: View {
    @AppStorage(MenuSettings.dataDirKey) private var dataDir = ""
    @AppStorage(MenuSettings.cliPathKey) private var cliPath = ""
    @AppStorage(MenuSettings.mountpointKey) private var mountpoint = ""

    var body: some View {
        Form {
            Section {
                PathRow(title: "Data folder", value: $dataDir,
                        placeholder: "~/Library/Application Support/GhostDrive",
                        chooseDirectory: true,
                        problem: MenuSettings.path(dataDir).flatMap { PathCheck.directory($0) })
                Text("Where the drive's keys, index and chunks live (--data).")
                    .font(.caption).foregroundColor(.secondary)
            }
            Section {
                PathRow(title: "ghostdrive tool", value: $cliPath,
                        placeholder: "found inside or next to the app, or on PATH",
                        chooseDirectory: false,
                        problem: MenuSettings.path(cliPath).flatMap { PathCheck.executable($0) })
                Text("The command-line tool the menu runs for every action.")
                    .font(.caption).foregroundColor(.secondary)
            }
            Section {
                PathRow(title: "Mountpoint", value: $mountpoint, placeholder: "~/GhostDrive",
                        chooseDirectory: true,
                        problem: MenuSettings.path(mountpoint).flatMap { PathCheck.directory($0) })
                Text("Where Start mounts the drive (--mount). Takes effect the next time it starts.")
                    .font(.caption).foregroundColor(.secondary)
            }
        }
        .padding(20)
        .frame(width: 560)
        .onAppear {
            // The app has no Dock icon, so bring the window forward explicitly.
            NSApp.activate(ignoringOtherApps: true)
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
        if let current = MenuSettings.path(value) {
            panel.directoryURL = URL(fileURLWithPath: current)
        }
        NSApp.activate(ignoringOtherApps: true)
        if panel.runModal() == .OK, let url = panel.url {
            value = url.path
        }
    }
}
