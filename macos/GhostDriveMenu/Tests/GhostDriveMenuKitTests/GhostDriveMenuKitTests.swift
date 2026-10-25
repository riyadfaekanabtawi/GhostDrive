import Foundation
import XCTest

@testable import GhostDriveMenuKit

final class CommandsTests: XCTestCase {
    func testStartPassesDataAndMountFromSettings() {
        let settings = MenuSettings(cliPath: "/opt/gd/ghostdrive", dataDir: "/Volumes/x/gd data",
                                    mountpoint: "/Users/me/My Drive")
        XCTAssertEqual(Commands.start(settings),
                       ["autostart", "on", "--data", "/Volumes/x/gd data",
                        "--mount", "/Users/me/My Drive"])
    }

    func testStartLeavesDefaultsToTheCLI() {
        XCTAssertEqual(Commands.start(MenuSettings()), ["autostart", "on"])
        XCTAssertEqual(Commands.start(MenuSettings(dataDir: "  ", mountpoint: "/m")),
                       ["autostart", "on", "--mount", "/m"])
        XCTAssertEqual(Commands.start(MenuSettings(dataDir: "/d")),
                       ["autostart", "on", "--data", "/d"])
    }

    func testTildeIsExpandedAndWhitespaceTrimmed() {
        let home = NSHomeDirectory()
        let settings = MenuSettings(dataDir: " ~/gd-data\n", mountpoint: "~/GhostDrive")
        XCTAssertEqual(Commands.start(settings),
                       ["autostart", "on", "--data", home + "/gd-data",
                        "--mount", home + "/GhostDrive"])
    }

    func testOtherCommandsPassOnlyTheDataFolder() {
        let settings = MenuSettings(dataDir: "/d", mountpoint: "/m")
        XCTAssertEqual(Commands.status(settings), ["status", "--json", "--data", "/d"])
        XCTAssertEqual(Commands.invite(settings), ["invite", "--data", "/d"])
        XCTAssertEqual(Commands.stop(settings), ["stop", "--data", "/d"])
        XCTAssertEqual(Commands.invite(MenuSettings()), ["invite"])
    }

    func testSettingsReadFromUserDefaults() throws {
        let suite = "org.ghostdrive.menu.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        defaults.set("/d", forKey: MenuSettings.dataDirKey)
        defaults.set("/m", forKey: MenuSettings.mountpointKey)
        defaults.set("/bin/ls", forKey: MenuSettings.cliPathKey)
        XCTAssertEqual(MenuSettings(defaults: defaults),
                       MenuSettings(cliPath: "/bin/ls", dataDir: "/d", mountpoint: "/m"))
    }
}

final class PathCheckTests: XCTestCase {
    private var dir: URL!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("ghostdrive-menu-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try FileManager.default.removeItem(at: dir)
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
        let settings = MenuSettings(cliPath: dir.path, dataDir: "rel", mountpoint: file.path)
        XCTAssertEqual(settings.problems(), [
            "ghostdrive tool: is a folder, not a program",
            "Data folder: must be a full path starting with / or ~",
            "Mountpoint: is a file, not a folder",
        ])
        XCTAssertEqual(settings.problems(includingMount: false).count, 2)
        XCTAssertEqual(MenuSettings().problems(), [])
    }

    func testMountPoint() {
        XCTAssertTrue(PathCheck.isMountPoint("/"))
        XCTAssertFalse(PathCheck.isMountPoint(dir.path))
        XCTAssertFalse(PathCheck.isMountPoint(dir.appendingPathComponent("missing").path))
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
    }
}
