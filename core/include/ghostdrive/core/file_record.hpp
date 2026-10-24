#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/core/version_vector.hpp"

namespace ghostdrive::core {

enum class Chunking : std::uint8_t {
    fixed = 0,
};

// One entry of the replicated index. `path` is drive-relative with '/' separators.
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
};

// Rejects empty, absolute, and dot-dot paths, empty components, and NUL bytes. Remote records
// pass through this before they reach the index or the filesystem.
bool is_valid_drive_path(std::string_view path) noexcept;

// Checks path validity and that the chunk list matches size and block size.
Status validate(const FileRecord& record);

// Syncthing-style conflict name: "dir/name.sync-conflict-YYYYMMDD-HHMMSS-DEVICE7.ext", built only
// from the losing version, so every device derives the same name independently.
std::string conflict_copy_path(std::string_view path, std::int64_t mtime_ns, std::uint64_t device);

}  // namespace ghostdrive::core
