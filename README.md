# GhostDrive

```text
      .-'''''-.
     /         \        ____ _               _   ____       _
    |  (o) (o)  |      / ___| |__   ___  ___| |_|  _ \ _ __(_)_   _____
    |     o     |     | |  _| '_ \ / _ \/ __| __| | | | '__| \ \ / / _ \
    |  .-----.  |     | |_| | | | | (_) \__ \ |_| |_| | |  | |\ V /  __/
    |  | ::: |  |      \____|_| |_|\___/|___/\__|____/|_|  |_| \_/ \___|
    |  '-----'  |
    \/\/\/\/\/\/\       a drive made of your own devices

    laptop <=== QUIC, end-to-end encrypted ===> desktop <=====> phone
```

GhostDrive is a virtual drive whose storage is your own devices. Each device mounts the same
drive. When you open a file that lives on another device, GhostDrive fetches only the byte ranges
being read, directly from that device over QUIC, end-to-end encrypted. There is no storage
server. The only server component is a rendezvous/relay you can host yourself; it forwards
encrypted packets and never stores file data.

Status: early, but usable (macOS with FUSE-T, Linux with libfuse3). Devices find each other on
the local network automatically (mDNS), and across the internet through a rendezvous server you
host, with hole punching and a relay fallback. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## How you use it

The drive appears as a normal volume (Finder sidebar, any app). The terminal is only for setup;
the menu-bar app (below) covers daily use, and QR pairing is planned.

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

Any device can invite, and a device invited by one is accepted by all the others.

### Removing a device

If a device is lost or stolen, remove it from any other device:

```
ghostdrive remove old-laptop   # by name, or by its ID (`ghostdrive id` on that device)
```

The other devices learn of it and stop accepting that device for good; to use it again, give
it a new identity (delete its data directory) and invite it afresh. If the removed device had
added devices you do not recognise, the command names them so you can remove those too. The
remaining devices also switch to new discovery keys, so the removed one can no longer spot them
on a local network or look up their addresses on the rendezvous server.

### Devices on different networks

Run the rendezvous server on any machine with a public IP that accepts UDP (a small VPS is
enough; platforms that only forward HTTP, such as Hugging Face Spaces, will not work):

```
ghostdrive-rendezvous --port 7374          # prints the server's fingerprint
```

Then, on a device of the drive:

```
ghostdrive rendezvous your.server.example:7374 <fingerprint>
```

Devices that join afterwards inherit the setting. They connect directly when hole punching
works and otherwise through the server's relay, which forwards only encrypted packets.

### Starting at login

```
ghostdrive autostart on        # start ghostdrived now and at every login
ghostdrive autostart status    # is it set up, is it running, and which ghostdrived?
ghostdrive autostart off       # stop it and remove the login item
```

On macOS this installs a LaunchAgent in `~/Library/LaunchAgents` (its log goes to
`ghostdrived.log` in the data directory); on Linux, a systemd user unit in
`~/.config/systemd/user`. It runs the `ghostdrived` next to the `ghostdrive` you ran, restarts
it if it crashes, and leaves it stopped after `ghostdrive stop` until the next login. `on`
accepts `--mount DIR`, and all three accept `--data DIR`, which gets its own login item.

### Menu-bar app

On macOS 13 or later, `macos/GhostDriveMenu` is a small menu-bar app. It shows this device,
which devices are online or offline, and whether the rendezvous server is connected, and it can
copy an invite code, open the drive in Finder, start GhostDrive and stop it.

```
cd macos/GhostDriveMenu
swift build                                     # or: swift run
swift test                                      # unit tests (no daemon needed)
GHOSTDRIVE_BUILD_DIR=../../build/release ./make-app.sh   # .build/GhostDrive Menu.app
```

The app does everything by running the `ghostdrive` tool (`status --json`, `invite`, `stop`,
`autostart on`). Settings (in the menu, or Command-comma) holds three values, each empty for
the default:

- Data folder: passed to every command as `--data`. Default:
  `~/Library/Application Support/GhostDrive`.
- ghostdrive tool: the CLI to run; it must exist and be executable. Default: inside or next to
  the app, then PATH, `/opt/homebrew/bin`, `/usr/local/bin` and `~/.local/bin`.
- Mountpoint: passed to Start as `--mount`. Default: `~/GhostDrive`.

They are stored in UserDefaults (`org.ghostdrive.menu`, keys `ghostdriveDataDir`,
`ghostdriveCLIPath`, `ghostdriveMountpoint`). The menu actions:

- Copy Invite Code runs `ghostdrive invite`, copies the `gd1-...` code (marked concealed for
  clipboard managers) and shows how long the CLI says it is valid, for example "Invite copied,
  valid 10 min".
- Open Drive in Finder is disabled unless the daemon reports a mountpoint that is really
  mounted.
- Start GhostDrive runs `ghostdrive autostart on [--data DIR] [--mount DIR]`, which creates a
  login item; it only runs when you click it. If a command fails, its error output is shown in
  the menu.

Device names and other text from the daemon are shown with control, bidirectional and other
invisible formatting characters replaced by `?`. Without `GHOSTDRIVE_BUILD_DIR`, `make-app.sh`
leaves the tools out of the bundle. The app never reads key files. `ghostdrive status --json`
is also usable from scripts.

## Limitations

- Across the internet, devices need a rendezvous server to find each other, and a relay when
  both sides are behind NAT types that block hole punching (symmetric NAT, CGNAT). You can run
  your own.
- A remote file is only readable while a device holding it is online.
- Streaming speed is bounded by the upload bandwidth of the device serving the data.
- MVP platforms are Linux and macOS. Windows is phase 2; iOS and Android are phase 3.
- A device that stays offline while more than three devices are removed may not find the
  others through the rendezvous server when it comes back; it reconnects once it sees one of
  them on the local network or reaches an address it has stored.
- MVP consistency is single writer per file. Concurrent edits from two devices produce a
  conflict copy rather than a merge.

## Installing

GhostDrive installs three programs: `ghostdrive` (the CLI), `ghostdrived` (the daemon) and
`ghostdrive-rendezvous` (the optional server). Keep `ghostdrive` and `ghostdrived` in the same
directory; `ghostdrive autostart on` runs the `ghostdrived` next to it. `ghostdrive --version`
prints the installed version.

Mounting needs FUSE-T on macOS (`brew install --cask fuse-t`) and libfuse3 on Linux (the
`libfuse3-3` or `fuse3` package). Install it before GhostDrive.

### From a release tarball

Each [release](https://github.com/riyadfaekanabtawi/GhostDrive/releases) has
`ghostdrive-<version>-<os>-<arch>.tar.gz` for macOS (arm64) and Linux (x86_64), with a
`.sha256` file next to it. The macOS build runs on macOS 13 or later (from 0.1.1; 0.1.0 may
need the macOS version it was built on). On Linux it needs libfuse3 and avahi-client.

```
shasum -a 256 -c ghostdrive-0.1.0-macos-arm64.tar.gz.sha256   # sha256sum -c on Linux
tar -xzf ghostdrive-0.1.0-macos-arm64.tar.gz
cp ghostdrive-0.1.0-macos-arm64/bin/* ~/.local/bin/            # or any directory on PATH
```

The macOS tarball also holds `GhostDrive Menu.app` with its own copy of `ghostdrive` and
`ghostdrived`; move it to `/Applications`. Its signature is ad-hoc, so macOS asks for
confirmation the first time it opens (Control-click, Open), and a browser download is
quarantined (`xattr -dr com.apple.quarantine "GhostDrive Menu.app"`). The Linux binaries also
need avahi-client (`libavahi-client3`) for LAN discovery.

### From Homebrew

The formula is in `packaging/homebrew/ghostdrive.rb`; put it in a tap of your own (it is not in
homebrew-core). It builds the tagged release from source, with vcpkg fetching and building the
pinned dependencies, so the first install takes a while.

```
brew install --cask fuse-t           # first: formulae cannot depend on casks
brew tap-new you/ghostdrive
cp packaging/homebrew/ghostdrive.rb "$(brew --repository you/ghostdrive)/Formula/"
brew install you/ghostdrive/ghostdrive
```

### From source

Build as described under [Building](#building), then install the release preset:

```
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix ~/.local     # bin/, plus share/doc/ghostdrive
```

`scripts/package.sh` does the release build and writes the tarball described above into
`build/release/`, including the menu-bar app on macOS; it writes nothing outside `build/release`.

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

LAN discovery on Linux needs the avahi-client development files (`libavahi-client-dev`) at build
time and a running avahi-daemon. Without them the daemon still runs and reaches devices by stored
addresses and the rendezvous server; the LAN tests skip when no avahi-daemon is reachable.

## License

Apache-2.0. See [LICENSE](LICENSE).
