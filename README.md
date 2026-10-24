# GhostDrive

GhostDrive is a virtual drive whose storage is your own devices. Each device mounts the same
drive. When you open a file that lives on another device, GhostDrive fetches only the byte ranges
being read, directly from that device over QUIC, end-to-end encrypted. There is no storage
server. The only server component is a rendezvous/relay you can host yourself; it forwards
encrypted packets and never stores file data.

Status: early implementation. The core library (identity, chunk store, metadata index) exists;
networking and the mount do not. Nothing is usable yet. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## How you use it

- A background daemon, `ghostdrived`, runs on each device and owns the mount.
- The drive appears as a normal folder or volume (Finder, Files, any file manager or app).
- A command-line tool, `ghostdrive`, pairs devices, shows status, and mounts or unmounts.
- A desktop menu-bar/tray app and mobile apps are planned; see the architecture document.

## Limitations

- Across the internet, devices need a rendezvous server to find each other, and a relay when
  both sides are behind NAT types that block hole punching (symmetric NAT, CGNAT). You can run
  your own.
- A remote file is only readable while a device holding it is online.
- Streaming speed is bounded by the upload bandwidth of the device serving the data.
- MVP platforms are Linux and macOS. Windows is phase 2; iOS and Android are phase 3.
- MVP consistency is single writer per file. Concurrent edits from two devices produce a
  conflict copy rather than a merge.

## Building

Requirements: CMake 3.25+, Ninja, a C++20 compiler (Apple clang 17 or GCC 13+ / clang 17+),
git. Dependencies (OpenSSL, BLAKE3, SQLite, GoogleTest) are built by vcpkg, which is pinned as a
git submodule.

```
git clone --recurse-submodules https://github.com/riyadfaekanabtawi/GhostDrive.git
cd GhostDrive
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Other presets: `release`, `asan` (AddressSanitizer + UBSan), `ubsan`. On macOS 26.5 with
Apple clang 17, every AddressSanitizer binary hangs at startup inside the sanitizer runtime;
use `ubsan` there.

The mount front end needs libfuse3 development headers on Linux (`libfuse3-dev`) or FUSE-T on
macOS (`brew install --cask macos-fuse-t/homebrew-cask/fuse-t`; free for non-commercial use).
Without them it is skipped and everything else still builds. Mount tests carry the `mount` label;
skip them with `ctest --preset default -LE mount` where mounting is not possible.

## License

Apache-2.0. See [LICENSE](LICENSE).
