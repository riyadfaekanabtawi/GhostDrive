import AppKit
import GhostDriveMenuKit
import SwiftUI

@main
struct GhostDriveMenuApp: App {
    @StateObject private var model = DriveModel()

    init() {
        // No Dock icon, also when run with `swift run` outside an app bundle (LSUIElement).
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    var body: some Scene {
        MenuBarExtra {
            MenuContent(model: model)
        } label: {
            Image(systemName: model.status == nil ? "externaldrive" : "externaldrive.fill")
                .accessibilityLabel("GhostDrive")
        }
        Settings {
            SettingsView()
        }
    }
}

struct MenuContent: View {
    @ObservedObject var model: DriveModel

    var body: some View {
        switch model.state {
        case .starting:
            Text("GhostDrive: checking...")
        case .noCLI:
            Text("ghostdrive command-line tool not found")
            Text("Install it on PATH, put it next to this app, or set it in Settings")
        case .badCLI(let reason):
            Text("The ghostdrive tool set in Settings cannot be used")
            Text(reason)
        case .notRunning(let reason):
            Text("GhostDrive is not running")
            if !reason.contains("not running") {
                Text(reason)
            }
        case .running(let status, _):
            RunningSection(status: status)
        }

        if !model.message.isEmpty {
            Divider()
            ForEach(Array(model.message.enumerated()), id: \.offset) { _, line in
                Text(line)
            }
        }

        Divider()
        if let status = model.status {
            Button("Copy Invite Code") { model.copyInvite() }
                .disabled(model.busy || status.removedBy != nil)
            Button(model.mountedAt == nil ? "Open Drive in Finder (not mounted)"
                                          : "Open Drive in Finder") { model.openInFinder() }
                .disabled(model.mountedAt == nil)
            Button("Stop GhostDrive") { model.stop() }
                .disabled(model.busy)
        } else if case .notRunning = model.state {
            Button("Start GhostDrive") { model.start() }
                .disabled(model.busy)
        }

        Divider()
        if #available(macOS 14, *) {
            SettingsLink { Text("Settings...") }
                .keyboardShortcut(",")
        } else {
            Button("Settings...") { model.openSettings() }
                .keyboardShortcut(",")
        }
        Divider()
        Text(versionText)
        Button("Quit GhostDrive Menu") { NSApplication.shared.terminate(nil) }
            .keyboardShortcut("q")
    }

    // From the bundle's Info.plist (make-app.sh); `swift run` has no bundle version.
    private var versionText: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
        if let version = version as? String, !version.isEmpty {
            return "GhostDrive Menu \(version)"
        }
        return "GhostDrive Menu"
    }
}

struct RunningSection: View {
    let status: DriveStatus  // already passed through DriveStatus.cleaned()

    var body: some View {
        Text("This device: \(status.device)")
        if let removedBy = status.removedBy {
            Text("Removed from the drive by \(removedBy)")
        }
        if let error = status.discoveryError {
            Text("Warning: could not replace the discovery key (\(error))")
        }
        Text(rendezvousText)

        Divider()
        let online = status.peers.filter(\.online)
        let offline = status.peers.filter { !$0.online }
        if status.peers.isEmpty {
            Text("No other devices yet")
        }
        if !online.isEmpty {
            Text("Online")
            ForEach(online) { peer in
                Text("  \(peer.name)\(peer.relayed ? " (relayed)" : "")")
            }
        }
        if !offline.isEmpty {
            Text("Offline")
            ForEach(offline) { peer in Text("  \(peer.name)") }
        }
    }

    private var rendezvousText: String {
        guard let rv = status.rendezvous else { return "Rendezvous: not set (local network only)" }
        if rv.connected {
            return "Rendezvous: connected to \(rv.address)"
        }
        return "Rendezvous: not connected to \(rv.address)"
    }
}
