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
    sequence, signer and signature, origin path (conflict copies), and a local effective
    version vector;
  - chunk maps: file version to an ordered list of chunk IDs;
  - devices: key, last acknowledged sequence, `have` info;
  - signers: short ID to key for every device ever pinned or known as removed, with a retired
    mark. Rows are never deleted, so a short ID stays bound to its first key.

  The scheme follows Syncthing BEP: each device has a monotonic local sequence plus an `index_id`.
  A peer sends `{index_id, max_sequence}` and receives only newer records.
- **Signed records.** The author in `modified_by` signs each record with its Ed25519 key over
  a context string, the drive ID, the path (the origin path for a conflict copy), size, mode,
  mtime, HLC, the canonical version vector, `modified_by`, block size, chunking, the deleted
  flag and the chunk list. Sequence and the holds flag are not signed. Relays never re-sign,
  so a member can neither forge a change in another device's name nor alter a record it
  relays. The index verifies a remote record only when it would change something (new, newer,
  or concurrent); ignored records cost no check. The rules:
  - the author's signature must verify against its key, which must be known here;
  - an author that is not retired must be pinned here. One that is unpinned but not removed
    (in doubt, or out because every device that vouched for it is out) signs nothing here
    until it is pinned again, and its records are never adopted;
  - a retired author's record counts only if adopted: it also carries the countersignature of
    a member pinned here, over another context string, the signed bytes and the author's
    signature. Covering the author's signature ties the countersignature to the exact record
    the author signed, so adoption can only pass on records the removed device really signed.
    The record itself is unchanged, so adoptions of one version by several members compare
    equal; the first one stored is kept. A retired author's record without a countersignature,
    or countersigned by a retired key, is rejected;
  - a record that breaks a rule for good is rejected as invalid; one whose author or adopter
    is unknown or not pinned here is rejected as possibly valid later.
- **Schema 2 migration.** Index schema 2 adds signatures. Opening a schema 1 index signs this
  device's own records as they are and gives the index a new `index_id`. Other devices'
  records stay unsigned: still readable here, never sent on or adopted, until a peer sends the
  same version signed by its author with the same content, author and HLC. Different content
  at that version is treated as a concurrent change, so both stay and only the signed one is
  sent. Trust on upgrade: schema 1 did not check `modified_by`, so a member could have planted
  a row in this device's name, and the migration signs it with this device's key. Schema 1
  stores no provenance to tell such rows apart; the exposure is one-time, at the upgrade.
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
  As built in step (d): through the system DNS-SD API (mDNSResponder on macOS, or an
  mDNSResponder libdns_sd on Linux; Avahi's compatibility library lacks shared connections and
  DNSServiceGetAddrInfo, so Linux builds against it skip LAN discovery), one shared connection
  driven by Asio. The TXT record is
  `v=1 id=<base32>`, where the ID is `HMAC-SHA256(drive_secret, "ghostdrive-lan-v1" || epoch
  || device key)` truncated to 12 bytes, with one-hour epochs; members check the neighbouring
  epochs too, for clock skew. A sighting updates the stored address and dials right away, in
  parallel with any dial to a stale address. Resolutions are capped (64 in flight, 32 per 10 s).
  The rotating ID hides which GhostDrive device advertises, but the SRV record points at the
  machine's normal `.local` host name, which macOS already advertises for other services; a
  random host record would hide that too and is left for later. IPv4 only for now.
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
- **As built in steps (d) and (e):**
  - The daemon holds one QUIC connection to its configured server, from the same UDP socket as
    everything else, so the address the server reports is that socket's NAT mapping. Every
    connection sends keep-alives after 15 s idle so mappings stay open.
  - Candidates are the server-observed address only. Host and port-mapped candidates and the
    sealed offers are not built; on one LAN, mDNS covers the host case.
  - `Connect{peer}` makes the server send `PeerAt` to both sides at once. Both send ten probes
    over two seconds to the other's observed address; the device with the smaller key dials.
  - If no direct connection exists `relay_after` (3 s) later, the smaller key dials the peer's
    virtual relay address instead. The order is direct first, then relay, the reverse of the
    plan above, which keeps the code simple for now.
  - Upgrade from relay to direct is by a new connection, not migration: devices keep asking the
    server for relayed peers, and a direct connection that succeeds replaces the relayed one
    (which is closed). A relayed connection never replaces a live direct one.
  - No NAT classification; the relay is the fallback for every hard case.
  - QUIC packets are at most 1400 bytes so the 37-byte relay header fits in a 1500-byte MTU.

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
- **As built in step (e)**, `ghostdrive-rendezvous`, with two changes from the plan above:
  - Client authentication is mutual TLS instead of the signed nonce. TLS 1.3 CertificateVerify
    already proves possession of the client key and binds it to the connection, which is what
    the nonce signature was for. The server therefore completes handshakes with any key (the
    one accept-any verifier outside tests) and serves nothing until the client registers.
  - Groups are `HMAC-SHA256(drive_secret, "ghostdrive-rendezvous-group-v1")` instead of a
    member-signed token: the server could not check such a signature anyway, since it does not
    know the members. Only members can compute the ID, and it says nothing about the secret.
  - Relay datagrams are raw UDP to the server's port: `0D 'G' 'D' 01 | 'R' | token(16) |
    key(32) | QUIC packet`. `Registered` gives each client a random 128-bit token, carried in
    every relay datagram in both directions. The server identifies the sender by its token (and
    requires the source to be that client's current path), never by source address alone, so
    an off-path spoofer can neither relay as a member nor spend its budget; it forwards only
    within one group, to the receiver's current path, with the receiver's token. Clients accept
    relay datagrams only from their server, with their own token, and only for member keys,
    and hand the inner packet to QUIC from a virtual address in 198.18.0.0/15 (bounded table);
    QUIC still authenticates the peer end to end.
  - New Initials get a Retry (RFC 9000 section 8.1): no state is created until the client
    returns the token from its own address, so spoofed Initials cannot fill handshake slots or
    use the server as a reflector. Handshakes time out after 5 s.
  - Limits: 10,000 clients, 16 per IPv4 address or IPv6 /64, 256 per group, registration
    within 10 s, 30 `Connect` per 10 s, 64 queued control frames per client (more are
    dropped), relayed payloads must be QUIC packets of at most 1400 bytes, and a per-client
    token bucket (4 MiB/s, 1 MiB burst) that drops instead of queueing. `--allow-group HEX`
    restricts the server to the operator's own drives; `ghostdrive status` shows the group.
  - Daemons act on a `PeerAt` for one peer at most every 2 s and run at most 32 dials at once,
    so even a malicious server cannot turn them into amplifiers. Probes carry no key. Active
    migration is disabled on every connection.
  - The group ID is a bearer credential, so since section 3.7 it is derived from the drive's
    current discovery key, which a removed device does not get. `Register` also carries an
    admission ID, the group of the drive secret, which never changes: `--allow-group` is
    checked against it (a 32-byte `Register` from an older client is its own admission ID).
    A removed device can still be admitted by such a server, but only to a group no member
    is in. The admission ID is the same value as the group before section 3.7, so existing
    allow lists keep working; it is also a stable drive identifier that a removed device can
    compute, so a server operator working with one could still link the drive's groups.
  - `Register` also lists up to 4 hidden groups, where the client may `Connect` to others but
    is never found itself, and relay forwarding is allowed between clients that share a group
    or where one is hidden in the other's. Only the visible group counts toward the 256 per
    group, since anyone who knows an earlier key could fill its group. A removed device can
    still have the server forward datagrams to a member hidden in its group; the member drops
    them (relayed packets are accepted only from members), so this costs bandwidth within the
    sender's relay budget and reveals nothing. A later `Register` on the same control stream moves
    the client to new groups and keeps its relay token, so relayed connections survive a
    change of discovery key.
  - Still open: relay headers carry public keys in clear, so an on-path observer
    can follow a device across networks; carrying relay traffic as QUIC DATAGRAM frames on the
    authenticated connections (RFC 9221) would close both this and the token's visibility.
  - Hosting needs UDP: a VPS or any host that forwards a UDP port. Platforms that only proxy
    HTTP(S) to one port, such as Hugging Face Spaces, cannot run it.

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

As built after step (c), before discovery (ahead of the planned order so the drive is usable):
- **Pairing by invite code** (the text form of the QR flow in section 2.3). `ghostdrive invite`
  asks the daemon for a code holding its full public key, a one-time 128-bit token, and its
  addresses. `ghostdrive join` pins that key, so only the inviter can complete the handshake,
  and sends the token. While an invite is open the inviter completes handshakes with unknown
  keys, but such a connection only reaches the pairing exchange: after a valid token (compared
  in constant time, consumed on use) the inviter pins the new key and sends `DriveInfo` (drive
  ID, drive secret, members). SAS comparison for pairing without a code is not built.
- **Membership** replicates as signed entries (`daemon/membership.hpp`), stored in
  `<data>/membership` and exchanged on one stream per direction of every member connection
  (frame type 13; an empty batch first, then new entries as they are recorded):
  - An entry is `add` (the author vouches that the subject belongs to the drive, with its
    name) or `remove` (the subject is out for good), Ed25519-signed by its author over a
    context string, the drive ID, and the body. Entries are kept only when their author is
    reachable by adds from this device; others wait in a bounded pool for their author's add.
  - Each device computes its own view: every known device is a member, out, or in doubt,
    settled to a fixpoint (a worklist with counters, linear per round). This device is a
    member. A device is out if it was ever removed here, if a member removes it, if every
    device vouching for it is out, or if no chain of vouches through devices that are not out
    leads to it from this device. It is a member if a member vouches for it and every device
    that removed it is out. Whatever is still in doubt is not a member. Consequences:
    - a remove counts only when its author is a member, and a device that a removed device
      vouches for, including keys it makes up afterwards and loops of such keys vouching for
      each other, is out;
    - removes of this device never count here (no member can make a device remove itself),
      and this device's own removes always do;
    - removal is permanent: a device that has stood removed by a member for
      `countersign_after` (at once if this device removed it) is recorded in
      `<data>/membership` for good and this device signs its own remove for it, so removing
      the remover later cannot bring it back, here or on a device that hears of both at once.
      The wait means a remove that a removed device gets to a device before news of its own
      removal does is undone when that news arrives, instead of becoming permanent;
    - a contest nobody settled (two devices removing each other, both seen at once) leaves
      both in doubt; `ghostdrive remove` accepts any known device, so the user settles it.
  - Pairing makes the first link both ways (inviter adds the new device, the new device adds
    the inviter, whose key the invite pinned). Each device then vouches for every member that
    has been one for `countersign_after` (60 s; a new device too, after joining), so members
    stay members when the device that invited them is removed, and a key that a just-removed
    device adds gets no vouch from a device that hears of the removal within that time.
  - The pinned devices in the index follow the view. A device that drops out of it is unpinned,
    its holdings are forgotten, it gets one last stream with the entries removing it (closed
    after 2 s whatever it does), and all its connections close. Membership is checked when a
    member connection starts, on every stream, after every index query, and on every frame
    received.
  - Names: this device's own vouch, else an inviting vouch by a member, else any vouch by a
    member. `ghostdrive status` shows device IDs; `ghostdrive remove` lists the devices the
    removed one vouched for that no other device invited (advice only: devices it made up can
    invite each other to hide from the list).
  - Drives from before this change vouch for their pinned devices when first opened.
  - Limits: 64 devices that are not out at once (counted for every add that would make a
    device live, not only new keys), 256 known keys not removed for good, names of at most 64
    bytes, and 32768 entries: 512 for this device's own, and 126 per other author, so the
    entries of all authors that are not out always fit. A full log is compacted at most once
    per change: entries by authors that are out and entries about devices removed for good are
    dropped (they affect no view), keeping this device's newest 256 removes so others still
    hear of them. Entries are stored only from authors that are not out here, a remove only
    for a known device, and the first entry for a statement wins; verified entries are not
    verified again. Entries that cannot be placed are dropped (the next connection resends
    everything).
  - Still open: any member can
    remove any other, and a malicious member can use up device slots (freed when it is
    removed); both deny service but never keep a removed device in. A malicious member that
    removes others more than `countersign_after` before its own removal reaches a device wins
    there, as any member's remove would. A member that churns through hundreds of removals
    can push old removes out of every device's log, so a device offline throughout may not
    hear of them. Devices a member vouched for while still trusted
    stay until removed by hand.
- **Index exchange** follows section 6. Each side opens one stream per connection and sends
  `Hello{index_id, max_sequence}`; the receiver answers `IndexRequest{after}` from its stored
  progress (0 if the index_id changed), and `IndexBatch` frames of up to 256 records follow,
  then new records as they are committed (polled every 250 ms). Each record says whether the
  sender holds every chunk of that version, which feeds `holders()` on the receiver.
  - Records carry their signature (section 3.1) in frame type 15; the unsigned type 6 is no
    longer sent or accepted, so an older peer fails cleanly. Unsigned records are not sent.
  - A rejected record is skipped for good, except one that may become valid later (its author
    or adopter is not established here yet: the membership stream can lag the index stream).
    Progress on that stream then stops just before the lowest such record, so the next
    connection sends it again; nothing is reset for other peers. This holds for
    `pending_hold` (10 minutes) per peer at most, counted from the first such record of each
    episode; an episode ends when a batch holds nothing back. After the hold the stream stops
    holding back, earlier batches included, and the records are skipped, but the (peer, device)
    pairs are stored (at most 1024, 64 per peer, the oldest evicted first). When that device
    is later pinned or retired, only those peers' progress is reset and their connections
    closed, so they send their index again. A device retired only on the inviter's word counts
    as not established yet, so its own records are held or skipped this way too. A record
    with sequence 0 is never held. When the membership view pins or retires a
    device, only the connections that delivered records naming it are closed (each remembers
    at most 64 devices). A batch still being applied then notices the change (an epoch taken
    before, checked against the devices it names after) and closes its connection itself.
  - A device removed for good (not one in doubt, and not one whose removal may still be
    undone) has its key retired as a signer: its stored records stay, new ones signed by it are
    rejected. Retiring runs before unpinning and again whenever the drive is opened, so a crash
    in between never leaves the key accepted. Each remaining member adopts, in the same
    transaction, the verified records the removed device signed and those it had countersigned
    (an adopter removed in turn): it countersigns each and changes nothing else, not the
    version, content, `modified_by` or the author's signature. A newer version by the removed
    device still supersedes an older one whoever adopted each, so adoption neither brings
    deleted files back nor makes conflict copies.
  - A device that joins later learns the retired keys from `DriveInfo` at pairing, since
    membership compaction may have dropped every entry naming them. These are provisional,
    on the inviter's word: they let adopted records be checked, but adopt nothing. A key the
    joiner sees as a member is not retired, and pinning one later clears the mark, so its
    own records verify normally. A key counts as retired for good only when this device's own
    membership view has it removed for good, which also adopts its records.
- **Data directory** (0700): `device.key` and `drive` (ID, secret, name) are 0600 and refused
  when readable by others, as are `membership` and `discovery` (section 3.7); `index.db`, `chunks/`, `staging/`, `peers` (last known address per
  member), and the control socket.
- **Control socket**: one text command per connection (`status`, `invite [ttl]`, `stop`,
  `ping`); reachable only by the owning user because of the directory and socket modes.
- **Limits on peers** (from the security review of this step): frames grow their buffer only
  as bytes arrive; at most two connections per member and eight pairing connections at once;
  block sizes must be powers of two in 128 KiB to 16 MiB; remote records whose clock is more
  than a day ahead or whose version counters near 2^62 are rejected, so neither can be wrapped;
  a member's claim to hold chunks counts only for the version actually stored; a holder that
  does not answer a GetChunk within 10 s is passed over. Still open:
  - a member can inflate other devices' counters in a vector it signs, hiding their later
    edits (attributable, not prevented);
  - a removed device's records, signed while it was a member, can be countersigned by any
    member; the content is the removed device's own, and the adopter is traceable;
  - an inviter can hand a new device keys as retired signers: until such a device is a member
    in the joiner's own view, the joiner accepts its records only as adopted;
  - a member colluding with a removed device (or holding its key) can countersign records
    newly made with that key; attribution then points at the removed device and the adopter;
  - any version a device once signed can be presented as that version's conflict copy, under
    the author's own signature (not attributable to the relay). A copy is valid only at the
    name derived from its origin, mtime and author, so this resurfaces old content under a
    `.sync-conflict-` name and cannot replace a file;
  - a member that sends records under keys never pinned anywhere holds that stream's
    progress back, so its records are fetched again on every connection;
  - adopting a removed device's records runs on the I/O thread in one transaction, about
    20 us of signing per record.
- **FUSE-T caching**: the macOS NFS client caches attributes and negative lookups, and FUSE-T
  cannot invalidate them (notifications are unsupported on its NFS backend). Mounts use
  `-o noattrcache`, and a directory's mtime is the time its entries last changed on this
  device, so remote changes appear within about a second. Unmounting goes through the kernel's
  own name for the mountpoint; FUSE-T's unmount alone leaves the mount behind when the given
  path differs in letter case, and polls for seconds when it does the unmount itself.


### 3.7 Discovery keys (built after membership)
- The drive secret keys the chunk hash, the LAN ID and the rendezvous group, and every device
  ever paired knows it. Changing the chunk key would mean rehashing every file, and buys
  nothing: chunk IDs only travel inside member connections. The LAN ID and the group are what
  let a removed device keep finding members, so they come from a separate discovery key.
- A discovery key is `{generation u64, author, key 32 bytes, Ed25519 signature by the author
  over "ghostdrive-discovery-key-v1" || drive_id || generation || author || key}`. Generation
  0 is the drive secret itself and is never sent. The current key is the one with the highest
  `(generation, author)` among the keys held from this device and from members; a device holds
  at most one key per author (the highest generation), so at most 65.
- Whenever the set of removed devices a device sees changes, it makes a new key one
  generation above every key it holds (from devices that are still members), saves it
  (`discovery`, 0600) and sends it on. Members exchange all the keys they hold on one stream
  per direction per connection (frame type 14, an empty list first, then the whole list
  whenever it changes), and pairing hands the new device the inviter's keys. Keys are only
  sent to members, checked right before each send, and only taken from members, with the
  author a member and the signature valid.
- Why a removed device ends up without the current key: as soon as a device sees the removal,
  keys by the removed device stop counting there (they are dropped), and it makes a key of its
  own above the rest. The removed device can still get a newer key from a member that has not
  heard of the removal, but that member makes yet another key when it does. The last keys
  made are by devices that already knew, and they never send them to it. Concurrent keys of
  one generation are settled by author, the same way everywhere.
- A received key may be at most 2^20 generations above the highest held before the frame, one
  key per author per frame. Without that cap a
  device could, before its removal, push the generations to the limit (2^62) so that nobody
  could make a key above one it knows.
- When the current key changes, the daemon re-registers on its existing rendezvous connection,
  in the new group, and updates its LAN advertisement. The last 3 keys that were current here,
  and the drive secret, are kept as earlier keys. The daemon is hidden in their groups (see
  section 3.5), so it finds a member that missed the change while a removed device that knows
  those keys cannot find it there.
- LAN sightings are matched against the current key first, then every other key held. A match
  on an older key may be a member that has not caught up, or a removed device posing as one
  (it knows the drive secret and every member's public key). Such a match only leads to a
  dial, which pins the member's key; it never replaces the stored address, and it is ignored
  for a member that has already sent a newer key over a connection. Such dials are limited to
  one per member per minute and four per 10 s in all, so forged sightings cannot take the
  dial slots.
- Drives from before this change start on the drive secret and make a key on first start if
  they already removed devices. A joining device takes the inviter's keys as already
  accounting for the removals it knows of.
- Limits: 65 keys per frame and per file plus 3 earlier keys, 16 KiB frames, generations
  below 2^62, steps of at most 2^20.
- A failure to make or save a key never fails the membership change that caused it;
  `ghostdrive status` shows a warning instead.
- Still open: a member that missed more than 3 changes of key is found only on the LAN or at
  an address it has stored. Until every member has heard of a removal, the removed device can
  still find the members that have not; a member that comes back in an old group can be found
  there by it until it catches up. A removed device can still make members on its LAN dial it
  by posing as a member that has not sent a newer key since the daemon started, which tells
  it that the dialing host belongs to the drive.

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
- Each record carries a version vector `[(device_short_id, counter)]`, signed with the rest of
  the record by its author (section 3.1).
  - If one record's vector dominates the other's, it is a normal update.
  - If neither dominates, the writes were concurrent. The winner by (HLC, device ID) keeps the
    name. The loser becomes `name.sync-conflict-YYYYMMDD-HHMMSS-<shortid>.ext`, which replicates
    like any other file. It keeps the loser's signature and records the original path, and is
    valid only at the name derived from that path, its mtime and its author.
  - The winner is stored and relayed exactly as its author signed it. Each device keeps a local
    effective vector, the merge of every version it has seen for the path, and compares
    incoming records and bumps its own edits against that. Every device picks the same winner,
    so they converge without anyone signing the merged vector.
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
| Malicious or buggy peer (member) | Serve wrong bytes; refuse to serve | Corrupt a reader (hash check before use); forge or alter a record in another device's name (signed records) |
| Stolen device | Read local chunks and the private key at rest | Stay in the drive after revocation; at-rest protection relies on OS disk encryption in the MVP |

Out of scope for the MVP:
- traffic analysis resistance (padding);
- compromised OS on a member device;
- friends' devices storing data (needs per-chunk encryption and an encrypted index; section 9).

Revocation:
1. A member removes a device's key (`ghostdrive remove`). The signed entry replicates and the
   key is unpinned everywhere (built; section 3.6).
2. Every remaining member makes a new discovery key and they converge on one the removed
   device never receives, so it can no longer recognise their LAN announcements or meet them
   in a rendezvous group (built; section 3.7).
3. The drive secret itself is not replaced: it still keys chunk IDs, which only travel inside
   member connections, so the removed device learns nothing new from knowing it.

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
