# GhostDrive Architecture

Status: decisions in section 11 accepted on 2026-10-24 (all recommendations taken). Claims tagged "(unverified)" were not confirmed against primary sources during research
and must be checked before code depends on them.

## 1. Goals and non-goals

Goals:
- One logical drive mounted on every device a user owns. Files physically live on one or more
  of those devices.
- Reading a remote file fetches only the chunks that cover the requested byte range, on demand,
  so seeking in a large video works without downloading it.
- All peer traffic is end-to-end encrypted and mutually authenticated by pinned Ed25519 keys.
- No storage server. A self-hostable rendezvous/relay helps peers find each other and forwards
  ciphertext when no direct path exists. It never stores file data.

Non-goals for the MVP:
- Multi-writer merging. Concurrent edits produce conflict copies.
- Storing data on other people's devices (friends). The design keeps room for it (section 9).
- Windows (phase 2) and mobile (phase 3), beyond keeping the C ABI boundary clean.

## 2. Corrections to the original requirements

These requirements were technically wrong or underspecified as written. The rest of this
document assumes the corrected version.

1. **"Single writer per file, last-writer-wins with conflict copies" is internally inconsistent.**
   If one writer were enforced there would be no concurrent versions and no conflicts. Without a
   lock server, offline devices cannot enforce a single writer. Correction: "single writer" is an
   advisory policy. Each file record carries a version vector, which detects when two devices
   wrote concurrently. LWW only decides which version keeps the canonical name. The other version
   is kept as a conflict copy, so no data is lost. The tie-break uses a hybrid logical clock
   (HLC), not raw wall-clock time, then device ID.
2. **"Fetches exactly the requested byte ranges".** Every chunk is integrity-checked against its
   content hash, and a partial chunk cannot be checked against a whole-chunk hash. Correction:
   fetch whole chunks covering the range (128 KiB minimum with the chunking scheme below). Exact
   sub-chunk fetches are possible later with BLAKE3 tree verification (Bao-style outboard
   encoding) (unverified detail); the chunk ID format leaves room for it. The kernel also does not
   issue application-sized reads: FUSE reads arrive in page and readahead units, and FUSE-T reads
   arrive as NFS-sized requests (32 KiB default rwsize), sometimes out of order.
3. **Pairing "by short code" needs more than a short code.** A 6-digit code sent through the
   rendezvous can be brute-forced or intercepted. Correction: in pairing mode, devices run the
   QUIC/TLS handshake without pinning, and each device shows a short authentication string (SAS)
   derived from the TLS exporter. The user confirms the two strings match, as in Bluetooth
   numeric comparison. A QR code carries the full key fingerprint plus a one-time 128-bit
   secret, so the user does not need to compare anything. The alternative is a PAKE (CPace or
   SPAKE2), which needs a crypto library OpenSSL does not provide.
4. **"Relay never sees plaintext" is achievable, but the relay is not blind.** It sees which
   device keys and IPs talk to each other, when, for how long, how many bytes, and packet sizes.
   The threat model states this explicitly (section 7).
5. **iOS range streaming is not guaranteed.** Apple's partial content fetching for File Provider
   (`NSFileProviderPartialContentFetching`) is documented for macOS 12.3+. iOS availability was
   not confirmed (unverified). If it is macOS-only, iOS apps opening a GhostDrive file get a fully
   materialized download. This affects phase 3 scope, not the MVP.
6. **QUIC is client/server; hole punching is simultaneous.** Both peers send at once, but each
   QUIC connection needs a client and a server. Roles are chosen deterministically (the smaller
   public key dials), and a collision rule handles both sides dialing.
7. **Loopback tests with latency and loss should not use tc/netem.** netem is Linux-only and
   needs root. The chosen QUIC library (ngtcp2) lets the application own the UDP socket, so
   impairment goes into our socket shim: delay, drop, reorder, with a seeded random source.
   Tests then run the same way on Linux and macOS without privileges.
8. **FUSE-T and macFUSE are not free for commercial redistribution.** FUSE-T needs a commercial
   license for commercial use. macFUSE forbids bundling it with commercial software without
   written permission. This constrains distribution and the license decision (section 11).

## 3. Components

```
+---------------------------- device ----------------------------+
|                                                                 |
|  Apps / Finder / file manager                                   |
|        |  POSIX read(), open(), stat()                          |
|  Kernel VFS -> FUSE (Linux) | NFS client via FUSE-T (macOS)     |
|        |                                                        |
|  vfs/        libfuse3 low-level adapter                         |
|        |  C++ calls (same process)                              |
|  core/       FileView (pinned version) -> ChunkCache -> Fetcher |
|              MetadataIndex (SQLite)   ChunkStore (files)        |
|              Identity (Ed25519, pinned peers)                   |
|        |                                                        |
|  transport/  QUIC (ngtcp2) connections, coroutine streams       |
|              UDP socket + demux (QUIC | probe | relay frames)   |
|        |                                                        |
|  discovery/  mDNS | rendezvous client | punching | relay client |
|                                                                 |
|  daemon/     ghostdrived: owns all of the above, control socket |
|  cli/        ghostdrive: talks to the control socket            |
|  capi/       C ABI over core (mobile front ends, later GUIs)    |
+-----------------------------------------------------------------+
            | UDP/QUIC (direct)          | QUIC or WSS (relayed)
            v                            v
      other devices              rendezvous/ (self-hosted)
                                 signalling + datagram relay
```

### 3.1 core/ (no networking)
- **Identity.** Each device has an Ed25519 keypair generated on first run. A device ID is the
  public key; the display form is a base32 fingerprint. Peers are pinned by public key in the
  drive membership list. The private key is stored in a 0600 file under the XDG data directory
  on Linux and in the Keychain on macOS (later).
- **Drive.** A drive has a random 256-bit drive ID and a drive secret, shared with members at
  pairing. The secret keys the mDNS rotating identifier and the keyed chunk hash (section 3.1,
  Chunk IDs).
- **Chunk store.** Content-addressed chunk files under `<data>/chunks/ab/cdef...`. Chunk bytes
  are never stored in the database. Writes go to a temp file, then rename. Reads are `pread`
  into pooled buffers.
- **Chunk IDs.** `algo-tag || BLAKE3-256 keyed by the drive secret over the plaintext`. Keying
  stops anyone outside the drive from testing whether a member holds a known file. The algorithm
  tag allows migrating to ciphertext-addressed chunks when friends' devices arrive.
- **Metadata index.** SQLite in WAL mode. Tables:
  - files: path, inode, size, mode, mtime, HLC, version vector, chunking params, deleted flag,
    sequence;
  - chunk maps: file version to an ordered list of chunk IDs;
  - devices: key, last acknowledged sequence, `have` info.

  The scheme follows Syncthing BEP: each device has a monotonic local sequence plus an `index_id`.
  A peer sends `{index_id, max_sequence}` and receives only newer records.
- **Chunk cache.** A bounded LRU of verified chunks held in refcounted pooled buffers. Buffers in
  use by a pending FUSE reply are pinned and cannot be evicted. Sequential access detection
  triggers readahead of the next N chunks, with N growing up to a cap while the stream stays
  sequential.
- **Fetcher.** Resolves a chunk ID to candidate peers: online devices whose index advertises that
  chunk as held. It ranks them LAN first, then by RTT, then by load. It requests the chunk, verifies
  its hash, retries another peer on mismatch or timeout, and coalesces concurrent requests for the
  same chunk. Because it talks to peers it lives in `vfs/` (core has no networking); the chunk
  cache stays in core. Ranking is not implemented yet: holders are tried in device key order.

### 3.2 transport/
- ngtcp2 (sans-IO) driven by our own event loop. One UDP socket per address family, shared by
  QUIC, hole-punch probes, and relay-encapsulated datagrams.
- **Demux on the first byte.** QUIC packets always have the fixed bit (0x40) set. Our probe and
  relay frames start with the magic prefix `0D 'G' 'D' 01` (last byte is the format version).
  0x0D has the fixed bit clear and lies in 4..15, which RFC 7983 and RFC 9443 leave unassigned,
  so it collides with neither STUN (0..3) nor DTLS (20..63). The 0x80-0xBF range was rejected
  because Version Negotiation packets may have the fixed bit clear. `grease_quic_bit` (RFC 9287)
  is never negotiated.
- **Coroutine API.** `co_await conn.open_stream()`, `co_await stream.read(std::span<std::byte>)`,
  `co_await stream.write(std::span<const std::byte>)`. ngtcp2 callbacks fire synchronously
  inside `ngtcp2_conn_read_pkt`/`writev_stream`. They only record events, and awaiting coroutines
  resume after the ngtcp2 call returns, never inside a callback. Each connection is confined to
  one strand.
- **TLS.** TLS 1.3 with a self-signed X.509 certificate carrying the device Ed25519 key. The verify
  callback accepts self-signed chains, and after the handshake the peer public key must equal a
  pinned key, otherwise the connection is closed. Raw public keys (RFC 7250) are a later option
  (OpenSSL 3.2+ API, unverified with QUIC). ALPN is `ghostdrive/1`.
- **Wire protocol.** Over the QUIC connection:
  - one long-lived control stream for hello, index exchange, and have-updates;
  - one bidi stream per chunk request: `GetChunk{chunk_id}` answered by `ChunkData{len, bytes}`
    or `Error{code}`.

  Frames are length-prefixed, with hand-written little-endian encoding and version-tagged messages.
- **Pre-authentication limits.** Incoming handshakes in progress are capped
  (`max_pending_handshakes`); `accept_peer` runs only after CertificateVerify. Still open: Retry
  (address validation) for internet-facing endpoints, and closing live connections when a key is
  unpinned (needed for revocation, section 7).
- **Impairment shim** for tests: delay, loss, reorder, and duplication on the socket shim, with a
  seeded random source.

### 3.3 vfs/
- libfuse3 **low-level** API (`fuse_lowlevel_ops`) on both Linux and macOS (FUSE-T). The
  low-level API is inode-based and allows replying to a `fuse_req_t` later from another thread,
  which is how async fetches complete without blocking a FUSE thread per read. Checked in step (c)
  against the FUSE-T 1.2.6 package: it ships libfuse 3.19 headers (an LGPL fork of osxfuse/libfuse)
  and `libfuse3.4.dylib` exports the whole low-level surface (`fuse_session_new_versioned`,
  `fuse_session_loop_mt`, `fuse_reply_iov`, `fuse_reply_data`, `fuse_lowlevel_notify_*`). The
  adapter uses `FUSE_USE_VERSION 35`, which Linux libfuse 3.5+ shares.
- `open` pins a file version (version vector plus chunk list) into the file handle, so a remote
  edit during streaming cannot mix versions. The next `open` sees the new version.
- MVP is read-mostly: writes go to a local staging file and are chunked and committed to the
  index on `flush` (every `close`), `fsync`, and the last `release`. Single writer is advisory;
  see section 2.1. As built in step (c):
  - The staging file is sparse and holds written bytes at their real offsets. A file's block
    size stays fixed once chosen, so a commit rehashes only the blocks a write touched; every
    other block keeps its chunk ID. A partial write to an untouched block first copies that
    block in, fetching it from a peer if needed, so editing a remote file moves one chunk.
    Truncating to zero lets the next commit choose the block size afresh.
  - Operations on one open file run in order on the io executor. While a file is open for
    writing, `stat` reports the size being written.
  - Empty directories are directory records (`mode` type bits `S_IFDIR`, no chunks); other
    directories are implied by file paths.
  - Rename keeps inode numbers, which FUSE-T's NFS client needs for open files. It commits the
    records at the new paths, then tombstones the old ones. That is not atomic across records:
    a crash in between leaves both copies, which loses nothing.
  - Unlinking an open file keeps its descriptor working, but nothing it writes is committed.
  - Not handled yet: macOS creates `._*` AppleDouble files and `.DS_Store` on network volumes,
    and these would replicate like any other file.

### 3.4 discovery/
- **LAN.** mDNS/DNS-SD service `_ghostdrive._udp`. The TXT record holds the port and a rotating ID
  `HMAC(drive_secret, epoch)` truncated, with a random instance name. Paired devices recognize
  each other; observers cannot track a device across epochs or learn its key. A forged
  advertisement only wastes one dial, because the TLS pin check rejects it. Announcements are
  rate-limited and the cache is capped.
- **WAN.**
  1. Each device keeps a QUIC connection to its rendezvous server.
  2. The server reports the observed address (server-reflexive candidate), using STUN or QUIC
     Address Discovery if the library supports it (unverified for ngtcp2).
  3. Candidate offers are sealed to the destination's key. The server routes them by key and
     cannot read them.
  4. Candidates are host addresses (including IPv6 global), server-reflexive, port-mapped
     (PCP, then NAT-PMP, then UPnP-IGD, later), and relay.
- **Connect relayed first, then upgrade.** A new connection starts over the relay immediately,
  while punching runs in the background, Tailscale/iroh style. When a direct path validates,
  QUIC connection migration (`ngtcp2_conn_initiate_migration`) moves the connection onto it. To
  QUIC the relay path looks like a virtual remote address.
- **Hard NAT.** The client probes two reflexive servers. If the mapped ports differ, the NAT
  maps per destination ("hard"). If both sides are hard, the client stays on the relay. No
  classic cone-type classification is done.

### 3.5 rendezvous/ (server binary)
- One self-hostable binary: signalling plus datagram relay. It is stateless apart from in-memory
  presence.
- **Client authentication.** The server sends a nonce. The client signs
  `"ghostdrive-relay-v1" || server_key || nonce || TLS exporter` with its Ed25519 key, which binds
  the proof to that connection.
- **Relaying.** `Send{dest_key, datagram}` is forwarded to `dest_key` if both keys are registered
  in the same drive group. Registration is a group token signed by a drive member, so the server
  never sees the drive secret.
- **Abuse controls.** A per-key token bucket, per-IP connection limits, and a maximum datagram
  size. Packets to slow receivers are dropped, not buffered. Nothing is written to disk.

### 3.6 daemon/, cli/, capi/
- `ghostdrived` owns the core, transport, discovery, and mount. It exposes a local control API
  over a Unix domain socket for pairing, status, peers, mount/unmount, and transfer stats.
- `ghostdrive` (CLI) is a thin client of that API. A future GUI (menu-bar or tray app) uses the
  same API. See section 11, Open decision 8.
- `capi/` is a C ABI over the core for in-process front ends (iOS File Provider extension,
  Android DocumentsProvider):
  - opaque handles;
  - caller-provided buffers;
  - integer error codes;
  - callbacks with `void* user` context;
  - no exceptions and no C++ types across the boundary;
  - a versioned header.

  It is designed in step (a) and used by vfs/ only through the same internal interfaces.

## 4. Data path of one read()

The scenario: an app on device A calls `read(fd, buf, 1 MiB)` at offset 300 MiB into `movie.mkv`.
The file lives only on device B.

1. **Kernel to FUSE.**
   - On Linux, the kernel splits the request into FUSE_READ requests of up to `max_read`, plus
     readahead.
   - On macOS with FUSE-T, the NFS client issues READs (32 KiB rwsize by default, possibly out
     of order). FUSE-T turns them into libfuse `read` calls.
2. **vfs/ to core.** The low-level `read(req, ino, size, off, fi)` handler looks up the pinned
   FileView in `fi->fh`. It calls `core::FileView::read(off, size, callback)` and returns
   without replying. The FUSE thread is released.
3. **Range to chunks.** For fixed-size chunking: `first = off / B`, `last = (off + size - 1) / B`.
   Here the block size `B` is 4 MiB, so the read needs chunk index 75. The chunk ID comes from the
   pinned chunk list.
4. **Cache.** If the chunk is in the cache, the handler goes to step 10. On a miss, it asks the
   Fetcher. The sequential detector notices the pattern and schedules readahead of chunks 76 and
   up in the background.
5. **Peer selection.** The Fetcher reads the location data from the index: device B holds the
   chunk and is online. B is reached directly over the LAN, or over a direct WAN path, or through
   the relay. With no live connection, transport asks discovery to connect (section 3.4).
6. **Request.** On a coroutine strand: `auto s = co_await conn.open_stream();`, then
   `co_await s.write(encode(GetChunk{id}))`.
7. **Remote side (B).** B's daemon accepts the stream, decodes the request, and checks that A is
   pinned (already done at the handshake). It `pread`s the chunk file into a pooled buffer and
   then writes it to the stream. ngtcp2 packetizes and encrypts it into the send buffer, and
   `sendmsg` puts it on the socket, using GSO where available.
8. **Network.** The datagrams travel either directly (UDP) or through the relay. On the relay
   path they are wrapped in `Send{dest_key, ...}` frames over B's relay connection and unwrapped
   on A's. The relay sees only ciphertext.
9. **Local receive (A).**
   1. `recvmsg` receives into a pooled buffer.
   2. The demux identifies the packet as QUIC.
   3. `ngtcp2_conn_read_pkt` decrypts it, and the `recv_stream_data` callback delivers plaintext
      stream bytes.
   4. The bytes are appended into the chunk's pooled buffer (sized from the `ChunkData` header).
   5. When the chunk is complete, its keyed BLAKE3 hash is checked against the ID. On mismatch
      the chunk is discarded, B is marked bad for that chunk, and the fetch retries elsewhere or
      fails.
10. **Cache insert and reply.**
    - The verified buffer goes into the LRU cache by refcount, with no copy.
    - The callback from step 2 runs on the core executor and calls `fuse_reply_buf(req, ptr,
      len)` (or `fuse_reply_data` with a `fuse_bufvec` on Linux) for the slice
      `[off - 75*B, +size]`.
    - The kernel copies the bytes into the app's `buf`.
11. **Timeouts.** Each FUSE request has a deadline (default 30 s, configurable). On expiry the
    handler replies `EIO` and the chunk fetch continues for the cache.

### 4.1 Copy points

Every copy of payload bytes on this path:

| # | Where | Why it cannot be avoided (MVP) |
|---|---|---|
| C1 | B: disk -> pooled buffer (`pread`) | Data must leave the page cache to be encrypted. |
| C2 | B: plaintext -> ngtcp2 packet buffer (encrypt) | AEAD writes ciphertext into the outgoing packet. |
| C3 | B: user -> kernel (`sendmsg`) | Kernel socket boundary. |
| C4 | Relay (relayed path only): recv buffer -> send buffer | Relay rewraps frames; could be zero-copy later with splice or io_uring. |
| C5 | A: kernel -> pooled buffer (`recvmsg`) | Kernel socket boundary. |
| C6 | A: ciphertext -> plaintext (decrypt) | ngtcp2 decrypts into its own buffer (`conn->crypto.decrypt_buf`, checked in ngtcp2 1.25.0). In-place decrypt is not exposed. |
| C7 | A: ngtcp2 stream data -> chunk buffer | Callback data is only valid during the callback, so it is reassembled into the chunk buffer. |
| C8 | A: chunk buffer -> kernel (FUSE reply) | `write`/`writev` to /dev/fuse. `fuse_reply_data` with splice can move pages on Linux (later). |
| C9 | A, macOS FUSE-T only: FUSE-T -> NFS reply -> kernel NFS client | Inside FUSE-T; outside our control. |
| C10 | A: kernel -> app buffer | Normal `read()` semantics. |

Cache hits skip C1 through C7. Code marks each of these with a `// COPY:` comment.

## 5. Concurrency model

- One I/O executor thread runs the event loop: UDP sockets, timers, QUIC, and coroutines. A small
  worker pool does disk reads, hashing, and SQLite work. Each QUIC connection is confined to one
  strand.
- libfuse runs its own multi-threaded session loop. Handlers post work to the core executor and
  return. Replies are sent from the executor.
- Cancellation: FUSE `INTERRUPT` and connection loss cancel the awaiting coroutine. A pending
  chunk fetch keeps going while other readers still need it.

## 6. Consistency and sync

- Index exchange works like Syncthing BEP. On connect, peers send `{index_id, max_sequence}` and
  receive newer records. Updates are pushed on the control stream.
- Each record carries a version vector `[(device_short_id, counter)]`.
  - If one record's vector dominates the other's, it is a normal update.
  - If neither dominates, the writes were concurrent. The winner by (HLC, device ID) keeps the
    name. The loser becomes `name.sync-conflict-YYYYMMDD-HHMMSS-<shortid>.ext`, which replicates
    like any other file.
- Deletes are tombstones with a version bump. A delete that is concurrent with an edit keeps the
  edit as a conflict copy.
  - Tombstones are garbage-collected once every member has acknowledged a sequence past them,
    with a long TTL for devices that never return.
  - Chunks are garbage-collected by reference count, after a grace period that covers pinned
    readers.
- Location data is derived from each device's index plus `have` information. It is not a
  separately replicated table.

## 7. Threat model

Assets: file contents, file names and tree, device private keys, the drive secret, and metadata
about who talks to whom.

| Adversary | Can | Cannot (by design) |
|---|---|---|
| Network attacker (passive or active) | See IPs, timing, sizes; drop, delay, or inject packets | Read or modify content (QUIC/TLS 1.3); impersonate a device (pinned keys) |
| Rendezvous/relay operator | See device keys, IPs, presence, traffic volume and timing; refuse service | Read content or file names; forge peers (end-to-end pin check); store data (no persistence) |
| Hostile LAN host | See mDNS announcements; spam them | Link announcements to a device over time (rotating ID); pass the pin check |
| Pairing MITM | Attempt to sit between two devices during pairing | Succeed without the user confirming a mismatched SAS (about 1 in 10^6 per attempt) or holding the QR secret |
| Malicious or buggy peer (member) | Serve wrong bytes; refuse to serve | Corrupt a reader (hash check before use) |
| Stolen device | Read local chunks and the private key at rest | Stay in the drive after revocation; at-rest protection relies on OS disk encryption in the MVP |

Out of scope for the MVP:
- traffic analysis resistance (padding);
- compromised OS on a member device;
- friends' devices storing data (needs per-chunk encryption and an encrypted index; section 9).

Revocation:
1. A member removes a device's key from the membership list. The change replicates and the key
   is unpinned everywhere.
2. The drive secret is rotated and redistributed to the remaining members.
3. Chunks keyed by the old secret stay valid until they are rewritten.

## 8. Failure modes

| Failure | Behavior |
|---|---|
| Peer goes offline mid-read | The stream errors or times out. The Fetcher retries any other device that holds the chunk. If none exists, the read returns `EIO` after the request deadline. Chunks already verified stay cached, so a retry after reconnect resumes at chunk granularity. |
| Path flaps (Wi-Fi to cellular, NAT rebinding) | QUIC migration or the peer's address-change handling keeps the connection. If the direct path dies, traffic falls back to the warm relay path immediately. Hysteresis prevents flapping back and forth. |
| Relay down | Direct connections are unaffected. Clients try configured fallback relays with jittered backoff. Peers behind hard NAT on both sides cannot reach each other until a relay returns. |
| Rendezvous down | Existing sessions and LAN (mDNS) keep working. Cached last-known candidates are tried directly. New WAN connections through hard NATs fail. |
| Unknown NAT type | Treated as hard. All candidates are tried in parallel while the relay path is already up, so connection never waits on classification. |
| Remote edit while streaming | The reader stays pinned to the version from `open`. If the old chunks were garbage-collected everywhere, the read returns `ESTALE`/`EIO`, and reopening sees the new version. |
| Hash mismatch | The chunk is discarded, the peer is marked bad for that chunk, the fetch retries elsewhere, and the event is logged with the peer ID (never the content). |
| Daemon crash | The mount reports `ENOTCONN`. On restart, SQLite WAL recovery runs, temp chunk files are deleted, and the FUSE mount is re-established. |
| Disk full on a source device | Writes fail with `ENOSPC`. Reads keep working. Cache eviction runs before reporting failure. |
| Clock skew | Ordering uses version vectors. HLC is only a tie-break for concurrent versions, so skew can only change which concurrent version keeps the name. |

## 9. Friends' devices (later, design constraints only)

When data is stored on devices that are not fully trusted:
- chunks need authenticated encryption at rest (for example XChaCha20-Poly1305);
- chunks are addressed by ciphertext hash;
- the index must be encrypted.

Plain convergent encryption leaks confirmation-of-file. Mixing in a per-drive convergence secret
limits dedupe to members (Tahoe-LAFS approach). The algorithm-tagged chunk ID (section 3.1)
exists so this does not need a format break.

## 10. Testing

- Unit tests: identity, chunk store, index (version vectors, conflicts, tombstones), cache, and
  frame codecs.
- Loopback integration tests: two daemons in one test process, or as child processes, on
  127.0.0.1 with the impairment shim (latency, loss, reorder, all seeded). Scenarios:
  - range reads;
  - readahead;
  - peer kill mid-read;
  - hash mismatch injection;
  - relay-only path through an in-process rendezvous;
  - migration from relay to direct.
- FUSE mount tests run where FUSE is available and are labelled so CI can skip them.

## 11. Decisions (accepted 2026-10-24)

The six decisions requested, each with a recommendation.

1. **macOS filesystem backend.** Recommend **FUSE-T** for the MVP, with the **macFUSE FSKit
   backend** (`-o backend=fskit`, macOS 15.4+) as a fallback.
   - Both use the libfuse3 API, so one vfs/ codebase serves Linux and macOS, and both deliver
     `read(offset, size)` directly.
   - Neither needs a kext or Recovery-mode changes. The macFUSE kext is rejected: on Apple
     Silicon it needs Reduced Security and two reboots.
   - Long term, a native **File Provider** replicated extension with
     `NSFileProviderPartialContentFetching` (macOS 12.3+) gives App Store eligibility and Finder
     integration, and shares work with the iOS extension. It materializes fetched ranges into
     sparse local files, so it needs its own eviction policy.
   - Costs: FUSE-T's NFS layer brings attribute caching, out-of-order reads, and reported
     reconnect hangs, and commercial use needs a FUSE-T license.
2. **QUIC library.** Recommend **ngtcp2** with the **OpenSSL 3.5+** crypto backend
   (`ngtcp2_crypto_ossl`), with wolfSSL as a tested fallback.
   - It is sans-IO, so the app owns the UDP socket. That is essential for hole punching, relay
     encapsulation, and the test impairment shim.
   - Other strengths: a clean fit for coroutines, connection migration, pure C on every platform
     including iOS and Android, an MIT license, and a vcpkg port.
   - Rejected: **msquic**, which owns its sockets and threads with no injection API, so hole
     punching would need a fork; and **quiche**, which is strong but BoringSSL-only, needs a
     Rust toolchain in the build, and has no vcpkg or Conan package.
   - Risks: the OpenSSL backend is labelled experimental in ngtcp2, there is no multipath, and
     we write the UDP loop (GSO, ECN, PMTU) ourselves.
3. **Chunking.** Recommend **fixed-size, power-of-two block size per file**, using the Syncthing
   rule:
   - the smallest size from 128 KiB to 16 MiB that keeps a file under 2000 blocks;
   - the block size stays fixed when the file grows.

   Offset-to-chunk lookup is O(1), which suits random range reads and media seeking. The index
   stores a per-file `chunking` field so content-defined chunking (FastCDC, min 256 KiB / avg
   1 MiB / max 4 MiB) can be added later for small mutable files without a format change.
4. **Dependency manager.** Recommend **vcpkg**, in manifest mode with a pinned baseline.
   - It has an ngtcp2 port with openssl/wolfssl/gnutls features; Conan Center has no ngtcp2
     recipe.
   - It has ports for asio, sqlite3, blake3, gtest, and openssl.
   - Its CMake toolchain integration works with presets, and its triplets cover iOS and Android
     for phase 3.
   - libfuse3 comes from the system (pkg-config) on Linux; FUSE-T is installed separately on
     macOS.
5. **Test framework.** Recommend **GoogleTest** (with gmock):
   - `gtest_discover_tests` integrates with ctest, and labels separate unit from integration
     tests;
   - gmock is useful for faking transport and discovery;
   - parameterized tests suit the impairment matrices.

   Catch2 would also work; GoogleTest has the edge for mocking.
6. **License.** Recommend **Apache-2.0** for all of GhostDrive:
   - it includes a patent grant;
   - it is compatible with OpenSSL 3 (Apache-2.0), ngtcp2 (MIT), SQLite (public domain), BLAKE3
     (CC0/Apache-2.0), and dynamically linked libfuse3 (LGPL-2.1).

   Alternatives:
   - **AGPL-3.0 for rendezvous/** only, if you want hosted forks of the server to publish
     changes;
   - **GPL-3.0** for everything, if you want copyleft.

   Independent of our license, distributing FUSE-T or macFUSE commercially needs their
   permission.

Further decisions that affect step (a) and (b). These are recommended defaults, and I will
proceed with them unless you object:

7. **Async runtime.** Standalone **Asio** with C++20 coroutines (`asio::awaitable`,
   `co_spawn`), providing UDP sockets, timers, and strands. GhostDrive's own awaitables (`stream.read`)
   are built on it. The alternative, a custom executor, means more code for no gain.
8. **User interface.** MVP is `ghostdrived` plus the `ghostdrive` CLI plus the mount itself. A
   menu-bar app (macOS, SwiftUI) or tray app (Linux) comes right after the MVP, as a thin client
   of the daemon's control socket. It is needed for QR pairing. Mobile apps are phase 3.
9. **Hash.** Keyed BLAKE3-256, from the official C implementation via vcpkg `blake3`.
10. **Index storage.** SQLite (WAL). Chunks are kept as files, not blobs.
11. **Pairing.** SAS numeric comparison in the MVP (CLI on both devices). The QR flow arrives
    with the GUI. A PAKE is not used in the MVP.
12. **Wire encoding.** Hand-written length-prefixed binary frames with version tags, with no
    protobuf or flatbuffers dependency.
13. **TLS backend.** OpenSSL 3.5+ from vcpkg. Checked in step (b): vcpkg pins OpenSSL 3.6.5 and
    ngtcp2 1.25.0, and TLS 1.3 with Ed25519 certificates works through `ngtcp2_crypto_ossl` (the
    loopback tests run mutual Ed25519 authentication).

## 12. Sources

QUIC:
- https://github.com/ngtcp2/ngtcp2, https://nghttp2.org/ngtcp2/programmers-guide.html
- https://vcpkg.io/en/package/ngtcp2
- https://github.com/cloudflare/quiche (quiche.h, releases)
- https://github.com/microsoft/msquic (docs/Platforms.md, docs/Execution.md, discussions/3550)

macOS filesystem:
- https://www.fuse-t.org/, https://github.com/macos-fuse-t/fuse-t (wiki, issues 96, 97, 109,
  113, 115, 123, License.txt)
- https://github.com/macfuse/macfuse/wiki/FUSE-Backends, macfuse LICENSE.txt
- https://developer.apple.com/documentation/fileprovider/nsfileproviderpartialcontentfetching
- https://developer.apple.com/documentation/fskit

NAT traversal and relay:
- https://tailscale.com/blog/how-nat-traversal-works, tailscale derp and netcheck packages
- https://iroh.computer/blog/qad, draft-ietf-quic-address-discovery
- libp2p specs: relay/DCUtR.md, relay/circuit-v2.md, autonat
- RFC 9000 section 9, RFC 8445, RFC 8305, RFC 6886, RFC 6887, RFC 6762/6763
- Apple TN3179 (local network privacy), Android NsdManager reference

Sync and metadata:
- https://docs.syncthing.net/specs/bep-v1.html, https://docs.syncthing.net/users/syncing.html
- FastCDC (USENIX ATC 2016), restic and borg chunker docs
- https://github.com/BLAKE3-team/BLAKE3-specs, https://github.com/oconnor663/bao
- https://tahoe-lafs.readthedocs.io/en/latest/convergence-secret.html

The references above were studied for design only. No code was copied.
