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
On Windows it mounts through [WinFsp](https://winfsp.dev), at a drive letter or a folder.

## How you use it

The drive appears as a normal volume (Finder sidebar, any app). The terminal is only for setup;
the desktop app (below) covers setup and daily use, and QR pairing is planned.

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

On Windows it registers a Task Scheduler task named `GhostDrive-<id>` that starts `ghostdrived`
when you log on, as you and without administrator rights; the Task Scheduler retries it up to
three times if it fails. The daemon runs in its own console window: minimise it, since closing
it stops GhostDrive as `ghostdrive stop` does. `--mount X:` mounts at a drive letter.

### Desktop app (macOS)

On macOS 13 or later, `macos/GhostDrive` builds GhostDrive.app: a window for setting up and
managing the drive, plus a menu-bar extra for quick actions.

```
cd macos/GhostDrive
swift build                                     # or: swift run GhostDriveApp
swift test                                      # unit tests (no daemon needed)
GHOSTDRIVE_BUILD_DIR=../../build/release ./make-app.sh   # .build/GhostDrive.app, tools inside
```

The window has these screens:

- Set up (only while the data folder holds no drive): "Create a new drive" runs
  `ghostdrive init --name NAME`; "Join with an invite code" runs `ghostdrive join CODE --name
  NAME`. Either then starts GhostDrive.
- Home: this device's name and ID, whether GhostDrive is running (Start, Stop), where the drive
  is mounted with an Open drive button, and the rendezvous server's state.
- Devices: the other devices from `ghostdrive status --json`, online, relayed or offline, with
  their IDs. Remove asks first, runs `ghostdrive remove ID` and shows what it printed, including
  any devices the removed one added, each with a button to remove it too.
- Add device: creates an invite (`ghostdrive invite`), shows the code large with Copy (marked
  concealed for clipboard managers) and a countdown, notices when a device has used it, and
  says what to do on the other device.
- Settings: Start or Stop GhostDrive, Start at login (`ghostdrive autostart on` or `off`), the
  rendezvous server (`ghostdrive rendezvous HOST:PORT FINGERPRINT`), and three folders, each
  empty for the default:
  - Data folder: passed to every command as `--data`. Default:
    `~/Library/Application Support/GhostDrive`.
  - Mount location: passed to the daemon as `--mount`. Default: `~/GhostDrive`.
  - ghostdrive tool: the CLI to run; it must exist and be executable. Default: inside or next
    to the app, then PATH, `/opt/homebrew/bin`, `/usr/local/bin` and `~/.local/bin`.

Start runs the `ghostdrived` next to that tool directly, logging to `ghostdrived.log` in the
data folder; it creates no login item, which only Start at login does. The menu-bar extra shows
this device, the other devices and the rendezvous state, and has Open GhostDrive, Copy Invite
Code, Open Drive in Finder, Start or Stop, and Settings. Closing the window keeps the menu-bar
extra running.

The settings are stored in UserDefaults (`org.ghostdrive.menu`, the identifier of the earlier
menu-bar app, keys `ghostdriveDataDir`, `ghostdriveCLIPath`, `ghostdriveMountpoint`; also
`ghostdrivePort`, not shown, the UDP port for Start and Join when running several drives on one
machine). The app polls `ghostdrive status --json` every few seconds and does everything through
the CLI; it never reads key files. Device names and other text from the daemon are shown with
control, bidirectional and other invisible formatting characters replaced by `?`. Without
`GHOSTDRIVE_BUILD_DIR`, `make-app.sh` leaves the tools out of the bundle.

### Windows app

`windows/GhostDriveApp` is a desktop app for Windows 10 (1809) or later on x64, in WinUI 3
(Windows App SDK 2.5.1, C# on .NET 10, supported until November 2028). It is unpackaged and
self-contained: the built folder runs as it is, with no MSIX, Windows App SDK runtime or .NET
install, and is meant to sit next to `ghostdrive.exe` or in a subfolder beside it. Published
trimmed, it is about 140 MB (55 MB zipped), most of it the bundled runtimes.

- First run: when the data folder has no drive, it offers "Create a new drive"
  (`ghostdrive init`) and "Join with an invite code" (`ghostdrive join`).
- Home: this device's name and ID, whether GhostDrive is running, where the drive is mounted
  (with Open drive) and the rendezvous state.
- Devices: the other devices, online, relayed or offline, each with Remove
  (`ghostdrive remove <ID>`, only after the dialog's Remove button; Cancel, Escape and Enter
  leave the device alone. Its output is shown, including devices the removed one added).
- Add device: a one-time invite code (`ghostdrive invite`), large and copyable (kept out of
  clipboard history), with the time it stays valid counting down.
- Settings: Start and Stop GhostDrive, Start at login (`ghostdrive autostart on/off`), the
  rendezvous server (`ghostdrive rendezvous`), and the data folder, mount location (a drive
  letter such as `G:` or a folder) and the `ghostdrive.exe` to use, each empty for the default.
  They are stored in `%LOCALAPPDATA%\GhostDriveApp\settings.json`.

It finds `ghostdrive.exe` next to itself or in the folder above unless Settings names one,
runs it without a shell
or console window, and checks `ghostdrive status --json` every three seconds. Start runs the
`ghostdrived.exe` next to `ghostdrive.exe` without a console window; Stop runs
`ghostdrive stop`. Text from the daemon is shown with control, bidirectional and other
invisible characters replaced by `?`. The app never reads key files or anything else in the
data folder; it only checks whether the folder's `drive` file exists.

Build it on Windows with the .NET 10 SDK pinned in `windows/GhostDriveApp/global.json`
(Visual Studio is not needed):

```
cd windows\GhostDriveApp
dotnet test GhostDriveApp.Core.Tests          # also runs on Linux and macOS
dotnet publish GhostDriveApp -c Release -o out
```

Then copy `ghostdrive.exe` and `ghostdrived.exe` from the `windows-release` build
(`build\windows-release\cli` and `build\windows-release\daemon`) into `out`, and run
`out\GhostDriveApp.exe`. The `windows` (Debug) build's programs need the vcpkg DLLs beside
them, so for those set `build\windows\cli\ghostdrive.exe` as the tool in Settings instead.

### Linux desktop app

`linux/ghostdrive-app` is a GTK 4 and libadwaita app. On first run it creates a drive or joins
one with an invite code; then it shows this device and whether GhostDrive is running (Home),
the other devices with Remove (Devices), a large invite code with its remaining validity (Add
device), and the data folder, mount location, rendezvous server, Start at login and Start/Stop
(Settings). It polls `ghostdrive status --json` every three seconds.

```
sudo apt install libgtk-4-dev libadwaita-1-dev     # GTK 4.10+, libadwaita 1.5+
cmake --preset default -DGHOSTDRIVE_BUILD_LINUX_APP=ON
cmake --build --preset default --target ghostdrive-app
PATH="$PWD/build/default/cli:$PATH" build/default/linux/ghostdrive-app/ghostdrive-app
```

Like the menu-bar app it does everything by running `ghostdrive` (next to the app, else on
PATH) with an argument vector, never a shell, and it never reads key files. Start runs the
`ghostdrived` next to the CLI with `--data` and `--mount`, logging to `ghostdrived.log` in the
data folder. Its settings are in `~/.config/ghostdrive/app.conf` (an empty value means the
CLI's default); `--data DIR` overrides the data folder for one session. Text from the daemon
is shown with control, bidirectional and invisible characters replaced by `?`. The command
building and parsing live in a library without GTK (`linux/ghostdrive-app/logic`), whose
tests run in the normal `ctest` on Linux and macOS. `cmake --install` also installs
`org.ghostdrive.GhostDrive.desktop` and its icon.

## Limitations

- Across the internet, devices need a rendezvous server to find each other, and a relay when
  both sides are behind NAT types that block hole punching (symmetric NAT, CGNAT). You can run
  your own.
- A remote file is only readable while a device holding it is online.
- Streaming speed is bounded by the upload bandwidth of the device serving the data.
- MVP platforms are Linux and macOS. Windows is phase 2; iOS and Android are phase 3. On
  Windows the drive mounts through WinFsp (`ghostdrived` mounts at `%USERPROFILE%\GhostDrive`,
  or `--mount X:` for a drive letter), finds devices on the local network through Windows' own
  mDNS (Windows 10 1809 or later), and `ghostdrive autostart` works. The Windows
  volume is case-insensitive: files from other devices whose names Windows cannot use (such as
  `a:b` or `CON`) are not shown there, and of names that differ only in case only one is shown
  (the one whose name sorts first, so `Report.txt` before `report.txt`); the others stay on the
  drive but cannot be reached from Windows.
- Saving on Windows to the exact name of such a hidden variant writes to the visible one:
  saving `report.txt` overwrites what Linux and macOS devices see as `Report.txt`. A folder
  mountpoint (`--mount C:\M\X`) is refused if another user could rename or change one of its
  existing parent folders; mount inside your user profile or at a drive letter instead.
- A device that stays offline while more than three devices are removed may not find the
  others through the rendezvous server when it comes back; it reconnects once it sees one of
  them on the local network or reaches an address it has stored.
- MVP consistency is single writer per file. Concurrent edits from two devices produce a
  conflict copy rather than a merge.

## Installing

GhostDrive installs a desktop app and three programs: `ghostdrive` (the CLI), `ghostdrived`
(the daemon) and `ghostdrive-rendezvous` (the optional server). The app does everything through
`ghostdrive`. Keep `ghostdrive` and `ghostdrived` in the same
directory; `ghostdrive autostart on` runs the `ghostdrived` next to it. `ghostdrive --version`
prints the installed version.

Mounting needs FUSE-T on macOS (`brew install --cask fuse-t`), libfuse3 on Linux (the
`libfuse3-3` or `fuse3` package) and [WinFsp](https://winfsp.dev) 2.x on Windows. Install it
before GhostDrive.

### From a release tarball

Each [release](https://github.com/riyadfaekanabtawi/GhostDrive/releases) has
`ghostdrive-<version>-<os>-<arch>.tar.gz` for macOS (arm64) and Linux (x86_64), with a
`.sha256` file next to it. Each holds its platform's desktop app, the three programs in
`bin/`, and an `INSTALL.txt`. Download the one for your system and check it:

```
shasum -a 256 -c ghostdrive-0.1.2-macos-arm64.tar.gz.sha256   # sha256sum -c on Linux
tar -xzf ghostdrive-0.1.2-macos-arm64.tar.gz                  # or double-click it in Finder
```

#### macOS

Open the extracted folder and drag `GhostDrive.app` to `/Applications`, then open it. It
needs macOS 13 or later (from 0.1.1; 0.1.0 may need the macOS version it was built on) and
FUSE-T. The app holds its own copy of `ghostdrive` and `ghostdrived`, so nothing else needs
installing. Its signature is ad-hoc, not from a registered developer, so macOS asks for
confirmation the first time it opens (Control-click, Open), and a browser download is
quarantined (`xattr -dr com.apple.quarantine /Applications/GhostDrive.app`). For the
terminal, copy the programs to a directory on PATH: `cp bin/* ~/.local/bin/`.

#### Linux

Releases after 0.1.1 include the GTK desktop app, `bin/ghostdrive-app`, with its menu entry
and icon. It needs these runtime packages (Ubuntu 24.04, Debian 13, Fedora 40 or later):

| Needed for | Library | Debian and Ubuntu | Fedora |
|---|---|---|---|
| The app | GTK 4.10+ | `libgtk-4-1` | `gtk4` |
| The app | libadwaita 1.5+ | `libadwaita-1-0` | `libadwaita` |
| Mounting | libfuse3 | `fuse3`, `libfuse3-3` (`libfuse3-4` on Debian 13) | `fuse3`, `fuse3-libs` |
| LAN discovery | avahi-client | `libavahi-client3` | `avahi-libs` |

```
cd ghostdrive-0.1.2-linux-x86_64
./install.sh      # into ~/.local/bin, ~/.local/share/applications and ~/.local/share/icons
```

`install.sh` installs for your user only, never uses sudo, and writes only under your home
directory (`$XDG_DATA_HOME` if it is set). Then open GhostDrive from the application menu.
The app also runs in place (`bin/ghostdrive-app`); keep it in the same folder as `ghostdrive`
and `ghostdrived`. `INSTALL.txt` in the tarball says how to remove it again.

### On Windows

Releases after 0.1.1 also have `ghostdrive-<version>-windows-x86_64.zip`, for Windows 10
(1809) or later on x64, with a `.sha256` file next to it. It holds one folder with
`ghostdrive.exe`, `ghostdrived.exe`, `ghostdrive-rendezvous.exe`, `LICENSE`, `README.md`, and
the desktop app in `app\`. The programs are linked statically, the Visual C++ runtime
included, and the app carries its own .NET and Windows App SDK runtimes, so nothing else needs
installing except [WinFsp](https://winfsp.dev) 2.x for mounting (the default installer options
are enough); `ghostdrived --no-mount` and `ghostdrive-rendezvous` run without it.

```
(Get-FileHash ghostdrive-0.1.2-windows-x86_64.zip -Algorithm SHA256).Hash   # compare with .sha256
Unblock-File ghostdrive-0.1.2-windows-x86_64.zip
Expand-Archive ghostdrive-0.1.2-windows-x86_64.zip -DestinationPath $env:LOCALAPPDATA\Programs
& "$env:LOCALAPPDATA\Programs\ghostdrive-0.1.2-windows-x86_64\app\GhostDriveApp.exe"
```

Start the app with `app\GhostDriveApp.exe`; it runs the `ghostdrive.exe` in the folder above,
so keep the folder as it is. The app is unpackaged and adds no Start menu entry; make a
shortcut to `GhostDriveApp.exe` if you want one. For the terminal, add the folder to PATH
(`$env:Path += ";<folder>"`, or in Settings). A browser download is marked as coming from the
internet, and neither the programs nor the app are signed, so SmartScreen may ask before the
first run (More info, Run anyway); `Unblock-File` on the zip before extracting avoids that.

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
`build/release/`, including GhostDrive.app on macOS and `ghostdrive-app` on Linux (which needs
`libgtk-4-dev` and `libadwaita-1-dev` to build); it writes nothing outside `build/release`.
On Windows, `pwsh scripts/package.ps1` does the same with the `windows-release` preset (static
`x64-windows-static-release` triplet), publishes the desktop app into `app\` with the .NET SDK
from `windows/GhostDriveApp/global.json`, and writes the zip into `build\windows-release\`.
Running the Release workflow by hand (`gh workflow run release.yml --ref <branch>`) builds and
checks all three packages and keeps them as workflow artifacts without publishing a release.

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

On Windows 10 or later, build from a Developer Command Prompt (or after running `vcvars64.bat`)
with Visual Studio 2022's C++ tools, its clang-cl component (or LLVM on PATH), CMake and Ninja:

```
.\external\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

The Windows mount needs [WinFsp](https://winfsp.dev) 2.x with its Developer feature (headers
and import library) at build time, found in `%ProgramFiles(x86)%\WinFsp` or `WINFSP_ROOT`, and
WinFsp installed at run time. Without the SDK the mount is skipped and `ghostdrived` needs
`--no-mount`. The e2e tests run in Git Bash.

LAN discovery on Linux needs the avahi-client development files (`libavahi-client-dev`) at build
time and a running avahi-daemon. Without them the daemon still runs and reaches devices by stored
addresses and the rendezvous server; the LAN tests skip when no avahi-daemon is reachable.

## License

Apache-2.0. See [LICENSE](LICENSE).
