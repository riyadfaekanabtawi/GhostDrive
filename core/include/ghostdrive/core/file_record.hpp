#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/core/version_vector.hpp"

namespace ghostdrive::core {

using DriveId = std::array<std::byte, 32>;

enum class Chunking : std::uint8_t {
    fixed = 0,
};

// One entry of the replicated index. `path` is drive-relative with '/' separators. The author in
// `modified_by` signs the record (record_signed_bytes); relays pass it on unchanged. A record
// whose author was removed from the drive also carries the countersignature of the member that
// adopted it (countersign_bytes).
struct FileRecord {
    std::string path;
    std::uint64_t size = 0;
    std::uint32_t mode = 0;
    std::int64_t mtime_ns = 0;
    std::uint64_t hlc = 0;
    VersionVector version;
    std::uint64_t modified_by = 0;
    std::uint32_t block_size = 0;
    Chunking chunking = Chunking::fixed;
    bool deleted = false;
    std::vector<ChunkId> chunks;
    std::uint64_t sequence = 0;  // local to each device's index; not replicated as-is
    // Set on a conflict copy: the path the loser's signature covers. The copy is valid only at
    // conflict_copy_path(origin_path, mtime_ns, modified_by).
    std::string origin_path;
    // Unset only for other devices' records stored before records were signed.
    std::optional<Signature> signature;
    // The adopting member and its countersignature; 0 and unset on any other record.
    std::uint64_t countersigner = 0;
    std::optional<Signature> countersignature;
    // Local only, never sent: every version this device has seen for the path merged, which
    // can be ahead of the signed `version` after a concurrent update.
    VersionVector effective_version;
};

// Rejects empty, absolute, and dot-dot paths, empty components, and NUL bytes. Remote records
// pass through this before they reach the index or the filesystem.
bool is_valid_drive_path(std::string_view path) noexcept;

// Checks path validity, that the chunk list matches size and block size, and that a conflict
// copy sits at the path derived from its origin.
Status validate(const FileRecord& record);

// Syncthing-style conflict name: "dir/name.sync-conflict-YYYYMMDD-HHMMSS-DEVICE7.ext", built only
// from the losing version, so every device derives the same name independently.
std::string conflict_copy_path(std::string_view path, std::int64_t mtime_ns, std::uint64_t device);

// The bytes the author's signature covers: a context string, the drive ID, the signed path
// (origin_path if set, else path), and every replicated field except sequence.
Bytes record_signed_bytes(const DriveId& drive, const FileRecord& record);
// The bytes a countersignature covers: another context string, record_signed_bytes and the
// author's signature. Covering the author's signature binds the countersignature to the exact
// record the author signed, so it vouches for nothing else.
Bytes countersign_bytes(const DriveId& drive, const FileRecord& record);

}  // namespace ghostdrive::core
