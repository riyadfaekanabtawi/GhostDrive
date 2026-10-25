import Foundation
import XCTest

@testable import GhostDriveKit

final class CommandsTests: XCTestCase {
    func testAutostartOnPassesDataAndMountFromSettings() {
        let settings = AppSettings(cliPath: "/opt/gd/ghostdrive", dataDir: "/Volumes/x/gd data",
                                   mountpoint: "/Users/me/My Drive")
        XCTAssertEqual(Commands.autostartOn(settings),
                       ["autostart", "on", "--data", "/Volumes/x/gd data",
                        "--mount", "/Users/me/My Drive"])
    }

    func testAutostartOnLeavesDefaultsToTheCLI() {
        XCTAssertEqual(Commands.autostartOn(AppSettings()), ["autostart", "on"])
        XCTAssertEqual(Commands.autostartOn(AppSettings(dataDir: "  ", mountpoint: "/m")),
                       ["autostart", "on", "--mount", "/m"])
        XCTAssertEqual(Commands.autostartOn(AppSettings(dataDir: "/d")),
                       ["autostart", "on", "--data", "/d"])
    }

    func testAutostartOffAndStatusPassOnlyTheDataFolder() {
        let settings = AppSettings(dataDir: "/d", mountpoint: "/m", port: "9000")
        XCTAssertEqual(Commands.autostartOff(settings), ["autostart", "off", "--data", "/d"])
        XCTAssertEqual(Commands.autostartStatus(settings), ["autostart", "status", "--data", "/d"])
        XCTAssertEqual(Commands.autostartOff(AppSettings()), ["autostart", "off"])
        XCTAssertEqual(Commands.autostartStatus(AppSettings()), ["autostart", "status"])
    }

    func testTildeIsExpandedAndWhitespaceTrimmed() {
        let home = NSHomeDirectory()
        let settings = AppSettings(dataDir: " ~/gd-data\n", mountpoint: "~/GhostDrive")
        XCTAssertEqual(Commands.autostartOn(settings),
                       ["autostart", "on", "--data", home + "/gd-data",
                        "--mount", home + "/GhostDrive"])
    }

    func testOtherCommandsPassOnlyTheDataFolder() {
        let settings = AppSettings(dataDir: "/d", mountpoint: "/m")
        XCTAssertEqual(Commands.status(settings), ["status", "--json", "--data", "/d"])
        XCTAssertEqual(Commands.invite(settings), ["invite", "--ttl", "600", "--data", "/d"])
        XCTAssertEqual(Commands.invite(settings, seconds: 120),
                       ["invite", "--ttl", "120", "--data", "/d"])
        XCTAssertEqual(Commands.stop(settings), ["stop", "--data", "/d"])
        XCTAssertEqual(Commands.id(settings), ["id", "--data", "/d"])
        XCTAssertEqual(Commands.invite(AppSettings()), ["invite", "--ttl", "600"])
        XCTAssertEqual(Commands.stop(AppSettings()), ["stop"])
    }

    func testInit() {
        XCTAssertEqual(Commands.initDrive(AppSettings(), name: "Riyad's Mac"),
                       ["init", "--name", "Riyad's Mac"])
        XCTAssertEqual(Commands.initDrive(AppSettings(dataDir: "/d", port: "9000"), name: "-x"),
                       ["init", "--name", "-x", "--data", "/d"])
    }

    func testJoinPassesThePort() {
        XCTAssertEqual(Commands.join(AppSettings(), code: "gd1-AAAA", name: "laptop"),
                       ["join", "gd1-AAAA", "--name", "laptop"])
        XCTAssertEqual(Commands.join(AppSettings(dataDir: "/d", port: " 24001 "),
                                     code: "gd1-AAAA", name: "laptop"),
                       ["join", "gd1-AAAA", "--name", "laptop", "--port", "24001",
                        "--data", "/d"])
        // A port that is not a number is left out (Settings reports it).
        XCTAssertEqual(Commands.join(AppSettings(port: "x"), code: "gd1-AAAA", name: "l"),
                       ["join", "gd1-AAAA", "--name", "l"])
    }

    func testRemoveByID() {
        XCTAssertEqual(Commands.remove(AppSettings(dataDir: "/d"), id: "AAAA-BBBB"),
                       ["remove", "AAAA-BBBB", "--data", "/d"])
    }

    func testRendezvous() {
        let server = RendezvousServer(address: "rv.example:7374", fingerprint: "AAAA-BBBB")
        XCTAssertEqual(Commands.rendezvous(AppSettings()), ["rendezvous"])
        XCTAssertEqual(Commands.setRendezvous(AppSettings(dataDir: "/d"), server),
                       ["rendezvous", "rv.example:7374", "AAAA-BBBB", "--data", "/d"])
    }

    func testDaemonArguments() {
        let home = NSHomeDirectory()
        XCTAssertEqual(Commands.daemon(AppSettings()),
                       ["--data", home + "/Library/Application Support/GhostDrive",
                        "--mount", home + "/GhostDrive"])
        XCTAssertEqual(Commands.daemon(AppSettings(dataDir: "/d", mountpoint: "/m", port: "24000")),
                       ["--data", "/d", "--mount", "/m", "--port", "24000"])
    }

    func testSettingsReadFromUserDefaults() throws {
        // The registration domain lives in memory only, so the test writes no preference file.
        let suite = "org.ghostdrive.app.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defaults.register(defaults: [AppSettings.dataDirKey: "/d", AppSettings.mountpointKey: "/m",
                                     AppSettings.cliPathKey: "/bin/ls", AppSettings.portKey: "24000"])
        XCTAssertEqual(AppSettings(defaults: defaults),
                       AppSettings(cliPath: "/bin/ls", dataDir: "/d", mountpoint: "/m",
                                   port: "24000"))
    }

    func testPort() {
        XCTAssertNil(AppSettings().portNumber)
        XCTAssertEqual(AppSettings(port: "7373").portNumber, 7373)
        XCTAssertNil(AppSettings(port: "0").portNumber)
        XCTAssertNil(AppSettings(port: "70000").portNumber)
        XCTAssertNil(AppSettings(port: "-1").portNumber)
        XCTAssertEqual(AppSettings(port: "x").problems(), ["Port: must be a number from 1 to 65535"])
        XCTAssertEqual(AppSettings(port: " ").problems(), [])
    }

    func testDefaultFolders() {
        XCTAssertEqual(AppSettings.defaultDataDir(home: "/Users/me"),
                       "/Users/me/Library/Application Support/GhostDrive")
        XCTAssertEqual(AppSettings.defaultMountpoint(home: "/Users/me"), "/Users/me/GhostDrive")
        // With nothing set, the app uses the user's folders, never one in a checkout.
        let home = NSHomeDirectory()
        XCTAssertEqual(AppSettings().effectiveDataDir, home + "/Library/Application Support/GhostDrive")
        XCTAssertEqual(AppSettings().effectiveMountpoint, home + "/GhostDrive")
        XCTAssertEqual(AppSettings(dataDir: " ", mountpoint: "").effectiveDataDir,
                       home + "/Library/Application Support/GhostDrive")
        XCTAssertEqual(AppSettings(defaults: UserDefaults(suiteName: "org.ghostdrive.empty.\(UUID())")!),
                       AppSettings())
        XCTAssertEqual(AppSettings(dataDir: "/d").effectiveDataDir, "/d")
        XCTAssertEqual(AppSettings(mountpoint: "/m").effectiveMountpoint, "/m")
    }
}

final class PathCheckTests: XCTestCase {
    private var dir: URL!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("ghostdrive-app-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try FileManager.default.removeItem(at: dir)
    }

    private func makeExecutable(_ url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try Data("#!/bin/sh\n".utf8).write(to: url)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path)
    }

    func testExecutable() throws {
        let tool = dir.appendingPathComponent("ghostdrive")
        try Data("#!/bin/sh\n".utf8).write(to: tool)
        XCTAssertEqual(PathCheck.executable(tool.path), .notExecutable)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tool.path)
        XCTAssertNil(PathCheck.executable(tool.path))
        XCTAssertEqual(PathCheck.executable(dir.path), .isDirectory)
        XCTAssertEqual(PathCheck.executable(dir.appendingPathComponent("nope").path), .missing)
        XCTAssertEqual(PathCheck.executable("bin/ghostdrive"), .notAbsolute)
    }

    func testDirectory() throws {
        let file = dir.appendingPathComponent("file")
        try Data().write(to: file)
        XCTAssertNil(PathCheck.directory(dir.path))
        XCTAssertNil(PathCheck.directory(dir.appendingPathComponent("not-yet").path))
        XCTAssertEqual(PathCheck.directory(file.path), .notDirectory)
        XCTAssertEqual(PathCheck.directory("relative"), .notAbsolute)
    }

    func testProblemsNameTheSetting() throws {
        let file = dir.appendingPathComponent("file")
        try Data().write(to: file)
        let settings = AppSettings(cliPath: dir.path, dataDir: "rel", mountpoint: file.path)
        XCTAssertEqual(settings.problems(), [
            "ghostdrive tool: is a folder, not a program",
            "Data folder: must be a full path starting with / or ~",
            "Mount location: is a file, not a folder",
        ])
        XCTAssertEqual(settings.problems(includingMount: false).count, 2)
        XCTAssertEqual(AppSettings().problems(), [])
    }

    func testMountPoint() {
        XCTAssertTrue(PathCheck.isMountPoint("/"))
        XCTAssertFalse(PathCheck.isMountPoint(dir.path))
        XCTAssertFalse(PathCheck.isMountPoint(dir.appendingPathComponent("missing").path))
    }

    func testHasDrive() throws {
        XCTAssertFalse(PathCheck.hasDrive(dataDir: dir.path))
        XCTAssertFalse(PathCheck.hasDrive(dataDir: dir.appendingPathComponent("missing").path))
        // A device key alone is what a join that did not finish leaves behind.
        try Data().write(to: dir.appendingPathComponent("device.key"))
        XCTAssertFalse(PathCheck.hasDrive(dataDir: dir.path))
        try Data().write(to: dir.appendingPathComponent("drive"))
        XCTAssertTrue(PathCheck.hasDrive(dataDir: dir.path))
    }

    func testDaemonNextToTheTool() throws {
        // Installed layout and app bundle: side by side.
        let bin = dir.appendingPathComponent("bin")
        try makeExecutable(bin.appendingPathComponent("ghostdrive"))
        XCTAssertNil(PathCheck.daemon(nextTo: bin.appendingPathComponent("ghostdrive")))
        try makeExecutable(bin.appendingPathComponent("ghostdrived"))
        XCTAssertEqual(PathCheck.daemon(nextTo: bin.appendingPathComponent("ghostdrive"))?.path,
                       bin.appendingPathComponent("ghostdrived").resolvingSymlinksInPath().path)

        // CMake build tree: cli/ghostdrive and daemon/ghostdrived.
        let build = dir.appendingPathComponent("build")
        try makeExecutable(build.appendingPathComponent("cli/ghostdrive"))
        try makeExecutable(build.appendingPathComponent("daemon/ghostdrived"))
        XCTAssertEqual(
            PathCheck.daemon(nextTo: build.appendingPathComponent("cli/ghostdrive"))?.path,
            build.appendingPathComponent("daemon/ghostdrived").resolvingSymlinksInPath().path)
    }
}

final class InviteTests: XCTestCase {
    // The exact text cmd_invite in cli/ghostdrive.cpp prints.
    private let output = """
        On the other device, run:

          ghostdrive join gd1-AEAQCAIB-AEAQCAIB-AEAQ

        The code works once, for 10 minutes. Anyone who has it can join, so send it privately.

        """

    func testParsesCodeAndValidity() {
        XCTAssertEqual(Invite.parse(output),
                       Invite(code: "gd1-AEAQCAIB-AEAQCAIB-AEAQ", minutes: 10))
        XCTAssertEqual(Invite.parse(output)?.confirmation,
                       "Invite copied, valid 10 min; it works once")
    }

    func testOtherValidity() {
        let text = output.replacingOccurrences(of: "for 10 minutes", with: "for 1 minute")
        XCTAssertEqual(Invite.parse(text)?.minutes, 1)
        let none = output.replacingOccurrences(of: "for 10 minutes", with: "for a while")
        XCTAssertEqual(Invite.parse(none), Invite(code: "gd1-AEAQCAIB-AEAQCAIB-AEAQ", minutes: nil))
        XCTAssertEqual(Invite.parse(none)?.confirmation, "Invite copied; it works once")
    }

    func testRejectsWhatIsNotACode() {
        XCTAssertNil(Invite.parse("ghostdrive: error: no drive"))
        XCTAssertNil(Invite.parse("ghostdrive join gd1-"))
        XCTAssertNil(Invite.parse("ghostdrive join gd1-ABC--DEF"))
        XCTAssertNil(Invite.parse("ghostdrive join gd1-abc18"))  // lower case, 1 and 8
        XCTAssertNil(Invite.parse("ghostdrive join gd1-AB\u{202E}CD"))
    }

    func testCodeFromWhatTheUserPasted() {
        XCTAssertEqual(Invite.code(inInput: "  gd1-AEAQ-CAIB\n"), "gd1-AEAQ-CAIB")
        XCTAssertEqual(Invite.code(inInput: "ghostdrive join gd1-AEAQ-CAIB"), "gd1-AEAQ-CAIB")
        XCTAssertNil(Invite.code(inInput: ""))
        XCTAssertNil(Invite.code(inInput: "gd1-aeaq"))
        XCTAssertNil(Invite.code(inInput: "hello"))
    }

    func testCountdown() {
        let created = Date(timeIntervalSince1970: 1000)
        XCTAssertEqual(Invite.remaining(created: created, seconds: 600, now: created), 600)
        XCTAssertEqual(Invite.remaining(created: created, seconds: 600,
                                        now: created.addingTimeInterval(55.5)), 545)
        XCTAssertEqual(Invite.remaining(created: created, seconds: 600,
                                        now: created.addingTimeInterval(601)), 0)
        XCTAssertEqual(Invite.countdown(600), "10:00")
        XCTAssertEqual(Invite.countdown(545), "9:05")
        XCTAssertEqual(Invite.countdown(0), "0:00")
        XCTAssertEqual(Invite.countdown(-3), "0:00")
    }
}

final class ParserTests: XCTestCase {
    func testInit() {
        // cmd_init in cli/ghostdrive.cpp.
        let text = """
            Created a new drive on probe.
              device ID: C6OI-K7IX-NOQ3-INHJ-2PYB-4BVC-LYZ7-J7RE-TCX2-BA4G-L65J-3XCC-ZK2A
              data:      /tmp/d

            Start it with `ghostdrived`, then add devices with `ghostdrive invite`.

            """
        XCTAssertEqual(InitOutput.parse(text),
                       InitOutput(name: "probe",
                                  id: "C6OI-K7IX-NOQ3-INHJ-2PYB-4BVC-LYZ7-J7RE-TCX2-BA4G-L65J-3XCC-ZK2A"))
        XCTAssertEqual(InitOutput.parse("Created a new drive on a\u{202E}b.\n  device ID: X\n")?.name,
                       "a?b")
        XCTAssertNil(InitOutput.parse("ghostdrive: a drive already exists at /tmp/d\n"))
    }

    func testJoin() {
        let text = """
            Contacting the inviting device...
            Joined the drive as laptop.
            Start it with `ghostdrived`; files appear in ~/GhostDrive.

            """
        XCTAssertEqual(JoinOutput.joinedName(text), "laptop")
        XCTAssertNil(JoinOutput.joinedName("Contacting the inviting device...\n"))
    }

    func testRemoveWithDevicesItAdded() {
        // Service::remove_device in daemon/src/service.cpp.
        let text = """
            Removed old (AAAA-BBBB). It can no longer connect to this drive's devices.
            friend (my phone) (CCCC-DDDD) was added by it. If you do not recognise it, remove it too: ghostdrive remove CCCC-DDDD
            evil\u{202E}x (EEEE) was added by it. If you do not recognise it, remove it too: ghostdrive remove EEEE

            """
        let out = RemoveOutput.parse(text)
        XCTAssertEqual(out.removed, .init(name: "old", id: "AAAA-BBBB"))
        XCTAssertEqual(out.addedByRemoved, [.init(name: "friend (my phone)", id: "CCCC-DDDD"),
                                            .init(name: "evil?x", id: "EEEE")])
        XCTAssertEqual(out.lines.count, 3)
        XCTAssertEqual(out.lines[2].prefix(8), "evil?x (")
    }

    func testRemoveAlone() {
        let out = RemoveOutput.parse(
            "Removed a (b) (AAAA). It can no longer connect to this drive's devices.\n")
        XCTAssertEqual(out.removed, .init(name: "a (b)", id: "AAAA"))
        XCTAssertEqual(out.addedByRemoved, [])
        XCTAssertEqual(RemoveOutput.parse("").lines, [])
        XCTAssertNil(RemoveOutput.parse("something else\n").removed)
    }

    func testRendezvousShown() {
        XCTAssertEqual(RendezvousServer.parseShown("none\n"), .some(nil))
        XCTAssertEqual(RendezvousServer.parseShown("rv.example:7374 AAAA-BBBB\n"),
                       .some(RendezvousServer(address: "rv.example:7374", fingerprint: "AAAA-BBBB")))
        // Outer nil: not something `rendezvous` prints.
        XCTAssertTrue(RendezvousServer.parseShown("ghostdrive: no drive\n") == nil)
        XCTAssertTrue(RendezvousServer.parseShown("") == nil)
        XCTAssertTrue(RendezvousServer.parseShown("rv.example AAAA\n") == nil)
    }

    func testRendezvousInput() {
        let fp = "C6OI-K7IX-NOQ3-INHJ-2PYB-4BVC-LYZ7-J7RE-TCX2-BA4G-L65J-3XCC-ZK2A"
        XCTAssertEqual(try RendezvousServer.fromInput(address: " rv.example:7374 ",
                                                      fingerprint: fp).get(),
                       RendezvousServer(address: "rv.example:7374", fingerprint: fp))
        // Pasted with spaces and a line break.
        let spaced = fp.replacingOccurrences(of: "-", with: " ") + "\n"
        XCTAssertEqual(try RendezvousServer.fromInput(address: "[::1]:7374",
                                                      fingerprint: spaced).get().fingerprint,
                       fp.replacingOccurrences(of: "-", with: ""))
        func error(_ a: String, _ f: String) -> RendezvousServer.InputError? {
            if case .failure(let e) = RendezvousServer.fromInput(address: a, fingerprint: f) {
                return e
            }
            return nil
        }
        XCTAssertEqual(error("", fp), .missingAddress)
        XCTAssertEqual(error("rv.example", fp), .badAddress)
        XCTAssertEqual(error(":7374", fp), .badAddress)
        XCTAssertEqual(error("rv.example:0", fp), .badAddress)
        XCTAssertEqual(error("rv.example:70000", fp), .badAddress)
        XCTAssertEqual(error("rv example:7374", fp), .badAddress)
        XCTAssertEqual(error("rv.example:7374", "AAAA"), .badFingerprint)
        XCTAssertEqual(error("rv.example:7374", fp.lowercased()), .badFingerprint)
    }

    func testAutostartStatus() {
        // cmd_autostart in cli/ghostdrive.cpp, macOS.
        let off = """
            login item: not set up
            launchd:    not loaded
            daemon:     /usr/local/bin/ghostdrived

            """
        XCTAssertEqual(AutostartState.parse(off),
                       AutostartState(enabled: false, loaded: false, daemonFound: true))
        let on = """
            login item: /Users/me/Library/LaunchAgents/org.ghostdrive.daemon.plist
            launchd:    loaded
            daemon:     not found

            """
        XCTAssertEqual(AutostartState.parse(on),
                       AutostartState(enabled: true, loaded: true, daemonFound: false))
        // Linux prints systemd: active.
        XCTAssertEqual(AutostartState.parse("login item: /u.service\nsystemd:    active\n")?.loaded,
                       true)
        XCTAssertNil(AutostartState.parse("ghostdrive: HOME is not set\n"))
    }
}

final class DisplayTextTests: XCTestCase {
    func testReplacesControlAndBidiCharacters() {
        XCTAssertEqual(DisplayText.clean("laptop"), "laptop")
        XCTAssertEqual(DisplayText.clean("Riyad's Mac\u{202E}gpj.exe"), "Riyad's Mac?gpj.exe")
        XCTAssertEqual(DisplayText.clean("a\u{2066}b\u{2069}c\u{200F}d\u{061C}e"), "a?b?c?d?e")
        XCTAssertEqual(DisplayText.clean("line\nbreak\r\tx\u{1B}[31m"), "line?break??x?[31m")
        XCTAssertEqual(DisplayText.clean("a\u{85}b\u{2028}c\u{2029}d\u{200B}e\u{FEFF}"),
                       "a?b?c?d?e?")
        XCTAssertEqual(DisplayText.clean("caf\u{E9} \u{6771}\u{4EAC}"), "caf\u{E9} \u{6771}\u{4EAC}")
    }

    func testCutsLongText() {
        let long = String(repeating: "x", count: 200)
        XCTAssertEqual(DisplayText.clean(long).count, DisplayText.maxLength)
        XCTAssertTrue(DisplayText.clean(long).hasSuffix("..."))
    }

    func testLines() {
        XCTAssertEqual(
            DisplayText.lines("ghostdrive: no drive at /x; run `ghostdrive init`\n\n  second \n"),
            ["no drive at /x; run `ghostdrive init`", "second"])
        XCTAssertEqual(DisplayText.lines("1\n2\n3\n4\n5\n6").count, 4)
        XCTAssertEqual(DisplayText.lines(""), [])
    }

    func testFailureShowsTheCLIErrorOutput() {
        // What `ghostdrive autostart on` prints when the data folder holds no drive.
        let stderr = "ghostdrive: no drive at /x; run `ghostdrive init` or `ghostdrive join` first\n"
        XCTAssertEqual(DisplayText.failure("Start failed", status: 1, stdout: "", stderr: stderr),
                       ["Start failed:",
                        "no drive at /x; run `ghostdrive init` or `ghostdrive join` first"])
        XCTAssertEqual(DisplayText.failure(nil, status: 1, stdout: "error: unknown\n", stderr: ""),
                       ["error: unknown"])
        XCTAssertEqual(DisplayText.failure("Stop failed", status: 2, stdout: "", stderr: ""),
                       ["Stop failed:", "ghostdrive exited with status 2"])
        XCTAssertEqual(DisplayText.failure(nil, status: 1, stdout: "", stderr: "1\n2\n3\n4\n5\n6",
                                           maxLines: 6).count, 6)
    }

    func testAppWording() {
        XCTAssertEqual(DisplayText.appWording("the GhostDrive daemon is not running (x)"),
                       "the GhostDrive daemon is not running (x)")
        XCTAssertEqual(DisplayText.appWording("Start it with `ghostdrived`."), "Start GhostDrive first.")
        // The second line cmd_join prints when joining fails.
        XCTAssertEqual(
            DisplayText.appWording("The invite may have expired or been used already; ask for a new "
                                   + "one with `ghostdrive invite`, and check that both devices are "
                                   + "on the same network."),
            "The invite may have expired or been used already; ask for a new one under Add device "
                + "on the other device, and check that both devices are on the same network.")
    }

    func testStatusIsCleaned() throws {
        let json = """
            {"device":"me\\u001b]0;x\\u0007","id":"ID","mountpoint":"","port":7000,
             "rendezvous":{"address":"rv\u{202E}.example:1","connected":true,"observed":null},
             "removed_by":null,"discovery_error":null,
             "peers":[{"name":"evil\u{202E}fdp.app","id":"P1","address":"10.0.0.2:7000",
                       "online":true,"relayed":false}]}
            """
        let status = try JSONDecoder().decode(DriveStatus.self, from: Data(json.utf8)).cleaned()
        XCTAssertEqual(status.device, "me?]0;x?")
        XCTAssertNil(status.mountpoint)
        XCTAssertEqual(status.rendezvous?.address, "rv?.example:1")
        XCTAssertEqual(status.peers.first?.name, "evil?fdp.app")
        XCTAssertEqual(status.rendezvousText, "Connected to rv?.example:1")
    }

    func testPeerStateAndRendezvousText() throws {
        let json = """
            {"device":"me","id":"ID","mountpoint":"/m","port":7000,
             "rendezvous":{"address":"rv:1","connected":false,"observed":null},
             "removed_by":null,"discovery_error":null,
             "peers":[{"name":"a","id":"A","address":null,"online":true,"relayed":false},
                      {"name":"b","id":"B","address":null,"online":true,"relayed":true},
                      {"name":"c","id":"C","address":null,"online":false,"relayed":false}]}
            """
        var status = try JSONDecoder().decode(DriveStatus.self, from: Data(json.utf8)).cleaned()
        XCTAssertEqual(status.peers.map(\.state), [.online, .relayed, .offline])
        XCTAssertEqual(status.mountpoint, "/m")
        XCTAssertEqual(status.rendezvousText, "Not connected to rv:1")
        status.rendezvous?.connected = true
        status.rendezvous?.observed = "1.2.3.4:5"
        XCTAssertEqual(status.rendezvousText, "Connected to rv:1, seen as 1.2.3.4:5")
        status.rendezvous = nil
        XCTAssertEqual(status.rendezvousText, "Not set (local network only)")
    }
}
