import AppKit
import GhostDriveKit
import SwiftUI

struct MainView: View {
    @ObservedObject var model: DriveModel
    @ObservedObject var navigation: Navigation

    // Before a drive exists only setting up and Settings make sense.
    private var sections: [Screen] {
        switch model.state {
        case .noDrive: return [.setup, .settings]
        default: return [.home, .devices, .addDevice, .settings]
        }
    }

    var body: some View {
        NavigationSplitView {
            List(sections, selection: $navigation.section) { section in
                Label(section.title, systemImage: section.icon).tag(section)
            }
            .navigationSplitViewColumnWidth(min: 160, ideal: 180)
        } detail: {
            if (navigation.section ?? sections[0]) == .settings {
                // A grouped Form scrolls by itself.
                SettingsView(model: model)
            } else {
                ScrollView {
                    detail
                        .frame(maxWidth: 640, alignment: .leading)
                        .padding(24)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
        .frame(minWidth: 680, minHeight: 440)
        .onAppear(perform: fixSection)
        .onChange(of: model.state) { _ in fixSection() }
    }

    @ViewBuilder private var detail: some View {
        switch navigation.section ?? sections[0] {
        case .setup: FirstRunView(model: model)
        case .home: HomeView(model: model, navigation: navigation)
        case .devices: DevicesView(model: model)
        case .addDevice: AddDeviceView(model: model)
        case .settings: SettingsView(model: model)
        }
    }

    private func fixSection() {
        if navigation.section == nil || !sections.contains(navigation.section!) {
            navigation.section = sections[0]
        }
    }
}

// MARK: - Shared pieces

struct ScreenTitle: View {
    let title: String
    var subtitle: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.largeTitle).bold()
            if let subtitle {
                Text(subtitle).foregroundStyle(.secondary)
            }
        }
        .padding(.bottom, 8)
    }
}

// The progress or result of an action, in plain words.
struct FeedbackView: View {
    let feedback: DriveModel.Feedback?
    var busy = false

    var body: some View {
        if let feedback, !feedback.lines.isEmpty {
            HStack(alignment: .top, spacing: 8) {
                if busy {
                    ProgressView().controlSize(.small)
                } else {
                    Image(systemName: feedback.ok ? "checkmark.circle" : "exclamationmark.triangle")
                        .foregroundStyle(feedback.ok ? Color.green : Color.orange)
                }
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(Array(feedback.lines.enumerated()), id: \.offset) { _, line in
                        Text(line).fixedSize(horizontal: false, vertical: true)
                    }
                }
                .textSelection(.enabled)
            }
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 8).fill(Color.secondary.opacity(0.1)))
        }
    }
}

struct StatusDot: View {
    let color: Color

    var body: some View {
        Circle().fill(color).frame(width: 9, height: 9)
    }
}

// Shown where a screen needs the daemon or the CLI.
struct NotAvailableView: View {
    @ObservedObject var model: DriveModel
    let what: String

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            switch model.state {
            case .starting:
                ProgressView().controlSize(.small)
            case .noCLI:
                Text("The ghostdrive command-line tool was not found. Install it on PATH, put it "
                     + "next to this app, or set it in Settings.")
            case .badCLI(let reason):
                Text("The ghostdrive tool set in Settings cannot be used: \(reason)")
            default:
                Text("GhostDrive is not running. Start it to \(what).")
                Button("Start GhostDrive") { model.start() }
                    .accessibilityLabel("Start GhostDrive")
                    .disabled(model.isBusy(.startStop))
                FeedbackView(feedback: model.feedback[.startStop],
                             busy: model.isBusy(.startStop))
            }
        }
    }
}

// MARK: - First run

struct FirstRunView: View {
    @ObservedObject var model: DriveModel
    @State private var mode = Mode.create
    @State private var name = FirstRunView.defaultName
    @State private var code = ""

    enum Mode: Hashable { case create, join }

    // The computer's name, as the CLI would pick it.
    static var defaultName: String {
        let host = Host.current().localizedName ?? ProcessInfo.processInfo.hostName
        return host.split(separator: ".").first.map(String.init) ?? "device"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            ScreenTitle(title: "Welcome to GhostDrive",
                        subtitle: "A drive made of your own devices. Files stay on them and travel "
                            + "between them end-to-end encrypted.")
            Picker("", selection: $mode) {
                Text("Create a new drive").tag(Mode.create)
                Text("Join with an invite code").tag(Mode.join)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(maxWidth: 420)

            Text(mode == .create
                 ? "Use this if GhostDrive is not set up on any of your devices yet."
                 : "Use this if another of your devices already has the drive. On that device, "
                     + "open Add device and create an invite code.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Form {
                TextField("Device name", text: $name, prompt: Text("for example Riyad's MacBook"))
                if mode == .join {
                    LabeledContent("Invite code") {
                        TextField("Invite code", text: $code, prompt: Text("gd1-..."))
                            .labelsHidden()
                            .font(.system(.body, design: .monospaced))
                    }
                }
            }
            .frame(maxWidth: 480)

            HStack {
                Button(mode == .create ? "Create Drive" : "Join Drive") {
                    if mode == .create {
                        model.create(name: name)
                    } else {
                        model.join(code: code, name: name)
                    }
                }
                .keyboardShortcut(.defaultAction)
                .accessibilityLabel(mode == .create ? "Create Drive" : "Join Drive")
                .disabled(model.isBusy(.setup) || name.trimmingCharacters(in: .whitespaces).isEmpty
                          || (mode == .join && code.trimmingCharacters(in: .whitespaces).isEmpty))
            }

            FeedbackView(feedback: model.feedback[.setup], busy: model.isBusy(.setup))

            Text("Data folder: \(model.settings.effectiveDataDir)")
                .font(.caption).foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
        .onChange(of: mode) { _ in model.clearFeedback(.setup) }
    }
}

// MARK: - Home

struct HomeView: View {
    @ObservedObject var model: DriveModel
    @ObservedObject var navigation: Navigation

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            ScreenTitle(title: model.status?.device ?? "GhostDrive",
                        subtitle: model.status == nil ? nil : "This device")
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 16, verticalSpacing: 12) {
                GridRow {
                    Text("Status").foregroundStyle(.secondary)
                    statusRow
                }
                GridRow {
                    Text("Device ID").foregroundStyle(.secondary)
                    Text(model.deviceID ?? "-")
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                }
                GridRow {
                    Text("Drive").foregroundStyle(.secondary)
                    driveRow
                }
                GridRow {
                    Text("Rendezvous").foregroundStyle(.secondary)
                    Text(rendezvousText)
                }
                if let status = model.status {
                    GridRow {
                        Text("Devices").foregroundStyle(.secondary)
                        HStack {
                            Text(devicesText(status))
                            Button("Show") { navigation.section = .devices }
                                .accessibilityLabel("Show devices")
                                .buttonStyle(.link)
                        }
                    }
                }
            }

            if let removedBy = model.status?.removedBy {
                Label("This device was removed from the drive by \(removedBy).",
                      systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
            }
            if let error = model.status?.discoveryError {
                Label("Could not replace the discovery key: \(error)",
                      systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
            }

            FeedbackView(feedback: model.feedback[.startStop], busy: model.isBusy(.startStop))
        }
        .onAppear { model.loadRendezvous() }
    }

    @ViewBuilder private var statusRow: some View {
        switch model.state {
        case .running:
            HStack {
                StatusDot(color: .green)
                Text("Running")
                Button("Stop") { model.stop() }.disabled(model.isBusy(.startStop))
                    .accessibilityLabel("Stop GhostDrive")
            }
        case .notRunning(let reason):
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    StatusDot(color: .secondary)
                    Text("Not running")
                    Button("Start GhostDrive") { model.start() }
                        .accessibilityLabel("Start GhostDrive")
                        .disabled(model.isBusy(.startStop))
                }
                if !reason.contains("not running") {
                    Text(reason).font(.caption).foregroundStyle(.secondary)
                }
            }
        case .starting:
            Text("Checking...")
        case .noCLI:
            Text("The ghostdrive command-line tool was not found; set it in Settings.")
        case .badCLI(let reason):
            Text("The ghostdrive tool set in Settings cannot be used: \(reason)")
        case .noDrive:
            Text("No drive yet")
        }
    }

    @ViewBuilder private var driveRow: some View {
        let shown = model.status?.mountpoint ?? model.settings.effectiveMountpoint
        HStack {
            Text(shown).textSelection(.enabled)
            Button("Open drive") { model.openDrive() }
                .accessibilityLabel("Open drive")
                .disabled(model.mountedAt == nil)
            if model.status != nil && model.mountedAt == nil {
                Text("(not mounted)").foregroundStyle(.secondary)
            }
        }
    }

    // While GhostDrive is not running, the configured server.
    private var rendezvousText: String {
        if let status = model.status { return status.rendezvousText }
        switch model.rendezvousSetting {
        case .some(.some(let server)): return "\(server.address) (not connected while stopped)"
        case .some(.none): return "Not set (local network only)"
        case .none: return "-"
        }
    }

    private func devicesText(_ status: DriveStatus) -> String {
        let online = status.peers.filter(\.online).count
        if status.peers.isEmpty { return "No other devices yet" }
        return "\(online) of \(status.peers.count) other devices online"
    }
}

// MARK: - Devices

struct DevicesView: View {
    @ObservedObject var model: DriveModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            ScreenTitle(title: "Devices", subtitle: "The other devices of this drive.")
            if let status = model.status {
                if status.peers.isEmpty {
                    Text("No other devices yet. Add one under Add device.")
                        .foregroundStyle(.secondary)
                } else {
                    VStack(spacing: 0) {
                        ForEach(status.peers) { peer in
                            PeerRow(peer: peer, disabled: model.isBusy(.remove)) {
                                model.requestRemoval(.init(name: peer.name, id: peer.id))
                            }
                            if peer.id != status.peers.last?.id { Divider() }
                        }
                    }
                    .background(RoundedRectangle(cornerRadius: 8)
                        .stroke(Color.secondary.opacity(0.25)))
                }
                FeedbackView(feedback: model.feedback[.remove], busy: model.isBusy(.remove))
                if let removal = model.removal {
                    ForEach(removal.addedByRemoved) { device in
                        Button("Remove \(device.name) too...") { model.requestRemoval(device) }
                            .accessibilityLabel("Remove \(device.name) too")
                            .disabled(model.isBusy(.remove))
                    }
                }
            } else {
                NotAvailableView(model: model, what: "see the devices")
            }
        }
        // Neither button is the default, so Return does nothing; Escape and Cancel cancel.
        .confirmationDialog(model.removalFlow.pending.map { "Remove \($0.name)?" } ?? "",
                            isPresented: Binding(get: { model.removalFlow.pending != nil },
                                                 set: { if !$0 { model.cancelRemoval() } }),
                            presenting: model.removalFlow.pending) { device in
            Button("Remove", role: .destructive) { model.confirmRemoval() }
                .accessibilityLabel("Remove")
            Button("Cancel", role: .cancel) { model.cancelRemoval() }
                .accessibilityLabel("Cancel")
        } message: { device in
            Text("\(device.name) will no longer be able to connect to this drive's devices. Use "
                 + "this for a lost or stolen device. This cannot be undone: to use it again, "
                 + "reset it and invite it afresh.\n\nID: \(device.id)")
        }
    }
}

struct PeerRow: View {
    let peer: DriveStatus.Peer
    let disabled: Bool
    let onRemove: () -> Void

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            StatusDot(color: color)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 8) {
                    Text(peer.name).font(.headline)
                    Text(label).font(.caption).foregroundStyle(.secondary)
                }
                Text(peer.id)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
            Button("Remove...", action: onRemove).disabled(disabled)
                .accessibilityLabel("Remove \(peer.name)")
        }
        .padding(12)
    }

    private var label: String {
        switch peer.state {
        case .online: return "Online"
        case .relayed: return "Online, relayed"
        case .offline: return "Offline"
        }
    }

    private var color: Color {
        switch peer.state {
        case .online: return .green
        case .relayed: return .yellow
        case .offline: return .secondary
        }
    }
}

// MARK: - Add device

struct AddDeviceView: View {
    @ObservedObject var model: DriveModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            ScreenTitle(title: "Add device",
                        subtitle: "Create a one-time code and enter it on the new device.")
            if let status = model.status {
                if status.removedBy != nil {
                    Text("This device was removed from the drive and cannot invite others.")
                } else if let invite = model.invite {
                    InviteCard(model: model, invite: invite)
                } else {
                    Button("Create Invite Code") { model.createInvite() }
                        .accessibilityLabel("Create invite code")
                        .keyboardShortcut(.defaultAction)
                        .disabled(model.isBusy(.invite))
                }
                FeedbackView(feedback: model.feedback[.invite], busy: model.isBusy(.invite))
                instructions
            } else {
                NotAvailableView(model: model, what: "add a device")
            }
        }
    }

    private var instructions: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("On the other device").font(.headline)
            Text("1. Install GhostDrive and open it.")
            Text("2. Choose \"Join with an invite code\", give the device a name and enter the code.")
            Text("   Or, in a terminal: ghostdrive join followed by the code.")
            Text("3. Keep both devices on the same network while it joins, unless a rendezvous "
                 + "server is set.")
            Text("The code works once. Anyone who has it can join, so send it privately.")
                .foregroundStyle(.secondary)
        }
        .fixedSize(horizontal: false, vertical: true)
    }
}

struct InviteCard: View {
    @ObservedObject var model: DriveModel
    let invite: DriveModel.ActiveInvite

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { context in
            let left = Invite.remaining(created: invite.created, seconds: invite.seconds,
                                        now: context.date)
            VStack(alignment: .leading, spacing: 12) {
                Text(invite.code)
                    .font(.system(size: 24, weight: .semibold, design: .monospaced))
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                    .foregroundStyle(left > 0 && model.joinedSinceInvite.isEmpty
                                     ? Color.primary : Color.secondary)
                    .accessibilityIdentifier("inviteCode")
                HStack(spacing: 12) {
                    Button("Copy") { model.copyInviteCode() }
                        .accessibilityLabel("Copy invite code")
                        .disabled(left == 0 || !model.joinedSinceInvite.isEmpty)
                    Button("New Code") { model.createInvite() }
                        .accessibilityLabel("New invite code")
                        .disabled(model.isBusy(.invite))
                    Button("Done") { model.discardInvite() }
                        .accessibilityLabel("Done with invite code")
                }
                if !model.joinedSinceInvite.isEmpty {
                    Label(model.joinedSinceInvite.joined(separator: ", ")
                          + " joined the drive. The code is used up.",
                          systemImage: "checkmark.circle")
                        .foregroundStyle(.green)
                } else if left > 0 {
                    Text("Valid for \(Invite.countdown(left))")
                        .font(.system(.title3, design: .monospaced))
                        .accessibilityIdentifier("inviteCountdown")
                } else {
                    Text("This code has expired. Create a new one.")
                        .foregroundStyle(.orange)
                }
            }
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color.secondary.opacity(0.1)))
        }
    }
}
