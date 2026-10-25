import AppKit
import GhostDriveKit
import SwiftUI

// One app: the main window (sidebar with Home, Devices, Add device and Settings) and the
// menu-bar extra with the quick actions. Closing the window leaves the menu-bar extra running
// and hides the Dock icon; "Open GhostDrive" brings the window back.
@main
struct GhostDriveApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = DriveModel()
    @StateObject private var navigation = Navigation()

    static let mainWindowID = "main"

    var body: some Scene {
        Window("GhostDrive", id: Self.mainWindowID) {
            MainView(model: model, navigation: navigation)
                .onAppear { NSApp.setActivationPolicy(.regular) }
                .onDisappear { NSApp.setActivationPolicy(.accessory) }
        }
        .defaultSize(width: 760, height: 520)
        .commands {
            CommandGroup(replacing: .appSettings) {
                SettingsCommand(navigation: navigation)
            }
            CommandGroup(replacing: .newItem) {}
        }

        MenuBarExtra {
            MenuContent(model: model, navigation: navigation)
        } label: {
            MenuBarLabel(running: model.status != nil)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillFinishLaunching(_ notification: Notification) {
        // Also when run with `swift run` outside an app bundle.
        NSApp.setActivationPolicy(.regular)
    }

    // The menu-bar extra keeps running without the window.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    // Clicking the Dock icon (or opening the app again) with the window closed shows it.
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        if !hasVisibleWindows { MainWindow.open?() }
        return true
    }
}

// Opens the main window from outside a view. Set by the menu-bar label, the one view that
// exists from launch whether or not the window does.
@MainActor
enum MainWindow {
    static var open: (() -> Void)?
    static var launchChecked = false

    static var isVisible: Bool {
        NSApp.windows.contains {
            $0.isVisible && $0.identifier?.rawValue.hasPrefix(GhostDriveApp.mainWindowID) == true
        }
    }
}

struct MenuBarLabel: View {
    let running: Bool
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Image(systemName: running ? "externaldrive.fill" : "externaldrive")
            .accessibilityLabel("GhostDrive")
            .onAppear {
                MainWindow.open = {
                    NSApp.setActivationPolicy(.regular)
                    openWindow(id: GhostDriveApp.mainWindowID)
                    NSApp.activate(ignoringOtherApps: true)
                }
                // Launching the app shows the window, also when macOS restored none. Scenes
                // are not ready at once, so look again a little later.
                for delay in [0.5, 1.5] {
                    DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                        if !MainWindow.launchChecked, !MainWindow.isVisible { MainWindow.open?() }
                        if delay > 1 { MainWindow.launchChecked = true }
                    }
                }
            }
    }
}

enum Screen: String, Hashable, CaseIterable, Identifiable {
    case setup, home, devices, addDevice, settings

    var id: String { rawValue }

    var title: String {
        switch self {
        case .setup: return "Set up"
        case .home: return "Home"
        case .devices: return "Devices"
        case .addDevice: return "Add device"
        case .settings: return "Settings"
        }
    }

    var icon: String {
        switch self {
        case .setup: return "sparkles"
        case .home: return "house"
        case .devices: return "laptopcomputer.and.iphone"
        case .addDevice: return "plus.circle"
        case .settings: return "gearshape"
        }
    }
}

@MainActor
final class Navigation: ObservableObject {
    @Published var section: Screen? = .home
}

// Opens the main window, optionally at a section, and brings the app forward.
struct WindowOpener {
    let openWindow: OpenWindowAction
    let navigation: Navigation

    @MainActor
    func callAsFunction(_ section: Screen? = nil) {
        if let section { navigation.section = section }
        NSApp.setActivationPolicy(.regular)
        openWindow(id: GhostDriveApp.mainWindowID)
        NSApp.activate(ignoringOtherApps: true)
    }
}

struct SettingsCommand: View {
    @Environment(\.openWindow) private var openWindow
    let navigation: Navigation

    var body: some View {
        Button("Settings...") {
            WindowOpener(openWindow: openWindow, navigation: navigation)(.settings)
        }
        .keyboardShortcut(",")
    }
}

struct MenuContent: View {
    @ObservedObject var model: DriveModel
    let navigation: Navigation
    @Environment(\.openWindow) private var openWindow

    private var open: WindowOpener { WindowOpener(openWindow: openWindow, navigation: navigation) }

    var body: some View {
        Button("Open GhostDrive") { open() }
        Divider()

        switch model.state {
        case .starting:
            Text("GhostDrive: checking...")
        case .noCLI:
            Text("ghostdrive command-line tool not found")
            Text("Install it on PATH, put it next to this app, or set it in Settings")
        case .badCLI(let reason):
            Text("The ghostdrive tool set in Settings cannot be used")
            Text(reason)
        case .noDrive:
            Text("No drive on this device yet")
            Button("Set Up GhostDrive...") { open(.setup) }
        case .notRunning(let reason):
            Text("GhostDrive is not running")
            if !reason.contains("not running") {
                Text(reason)
            }
        case .running(let status, _, _):
            RunningSection(status: status)
        }

        ForEach([DriveModel.Action.menu, .startStop], id: \.self) { action in
            if let feedback = model.feedback[action] {
                Divider()
                ForEach(Array(feedback.lines.enumerated()), id: \.offset) { _, line in
                    Text(line)
                }
            }
        }

        Divider()
        if let status = model.status {
            Button("Copy Invite Code") { model.createInvite(copy: true) }
                .disabled(model.isBusy || status.removedBy != nil)
            Button(model.mountedAt == nil ? "Open Drive in Finder (not mounted)"
                                          : "Open Drive in Finder") { model.openDrive() }
                .disabled(model.mountedAt == nil)
            Button("Stop GhostDrive") { model.stop() }
                .disabled(model.isBusy)
        } else if case .notRunning = model.state {
            Button("Start GhostDrive") { model.start() }
                .disabled(model.isBusy)
        }

        Divider()
        Button("Settings...") { open(.settings) }
            .keyboardShortcut(",")
        Divider()
        Text(versionText)
        Button("Quit GhostDrive") { NSApplication.shared.terminate(nil) }
            .keyboardShortcut("q")
    }

    // From the bundle's Info.plist (make-app.sh); `swift run` has no bundle version.
    private var versionText: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
        if let version = version as? String, !version.isEmpty {
            return "GhostDrive \(version)"
        }
        return "GhostDrive"
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
        Text("Rendezvous: " + status.rendezvousText)

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
}
