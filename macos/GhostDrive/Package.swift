// swift-tools-version:5.7
// 5.7 is the first tools version that knows .macOS(.v13), the floor for MenuBarExtra and the
// Window scene, and matches Xcode 14.1, the first Xcode with the macOS 13 SDK. Nothing newer is
// needed, and the Swift 5 language mode it implies builds unchanged on current toolchains.

import PackageDescription

let package = Package(
    name: "GhostDrive",
    platforms: [.macOS(.v13)],
    targets: [
        // Everything that can be tested without AppKit: settings, the CLI argument lists, and
        // parsing of what the CLI prints.
        .target(name: "GhostDriveKit", path: "Sources/GhostDriveKit"),
        // Not "GhostDrive": on a case-insensitive file system that name would clash with the
        // ghostdrive tool bundled next to it.
        .executableTarget(name: "GhostDriveApp", dependencies: ["GhostDriveKit"],
                          path: "Sources/GhostDriveApp"),
        .testTarget(name: "GhostDriveKitTests", dependencies: ["GhostDriveKit"],
                    path: "Tests/GhostDriveKitTests"),
    ]
)
