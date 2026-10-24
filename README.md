# GhostDrive

GhostDrive is a virtual drive whose storage is your own devices. Each device mounts the same
drive. When you open a file that lives on another device, GhostDrive fetches only the byte ranges
being read, directly from that device over QUIC, end-to-end encrypted. There is no storage
server. The only server component is a rendezvous/relay you can host yourself; it forwards
encrypted packets and never stores file data.

Status: early, but usable between devices on the same network (macOS with FUSE-T, Linux with
libfuse3). Devices are reached by the address in the invite code; finding each other across the
internet (rendezvous, hole punching, relay) is not built yet. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## How you use it

The drive appears as a normal volume (Finder sidebar, any app). The terminal is only for setup;
a menu-bar app with QR pairing is planned.

On the first device:

```
ghostdrive init              # create a drive
ghostdrived &                # run it; the drive mounts at ~/GhostDrive
ghostdrive invite            # prints a one-time code
```

On each other device (same network):

```
ghostdrive join gd1-...      # the code from `invite`
ghostdrived &
```

Files you put in `~/GhostDrive` on one device appear on the others. Opening a file stored on
another device fetches just the parts being read. `ghostdrive status` lists the devices and
whether they are online; `ghostdrive stop` unmounts and stops the daemon.

The invite code contains the inviting device's full key, so the new device cannot be tricked
into joining an impostor; anyone holding the code can join until it is used or expires (10
minutes by default), so send it privately.

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
