# GhostDrive

GhostDrive is a virtual drive whose storage is your own devices. Each device mounts the same
drive. When you open a file that lives on another device, GhostDrive fetches only the byte ranges
being read, directly from that device over QUIC, end-to-end encrypted. There is no storage
server. The only server component is a rendezvous/relay you can host yourself; it forwards
encrypted packets and never stores file data.

Status: design stage. Nothing is usable yet. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

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

Requirements are fixed after the design checkpoint. Expected:

```
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Linux needs libfuse3 development headers. macOS needs FUSE-T.

## License

To be decided; see docs/ARCHITECTURE.md.
