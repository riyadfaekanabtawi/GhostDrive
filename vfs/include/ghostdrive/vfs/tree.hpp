#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ghostdrive/core/file_record.hpp"
#include "ghostdrive/core/metadata_index.hpp"
#include "ghostdrive/core/result.hpp"

namespace ghostdrive::vfs {

using Ino = std::uint64_t;
inline constexpr Ino kRootIno = 1;

enum class NodeType { directory, file };

struct Attr {
    Ino ino = 0;
    NodeType type = NodeType::file;
    std::uint32_t perm = 0;  // permission bits only
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint32_t nlink = 1;
};

struct DirEntry {
    std::string name;
    Ino ino = 0;
    NodeType type = NodeType::file;
};

// Inode table over the metadata index. Directories are implied by file paths and disappear
// with their last file. Inode numbers are never reused, so a stale inode held by the kernel
// can only resolve to nothing, never to a different file. Thread-safe.
class Tree {
public:
    Tree();

    // Applies index records newer than the last refresh. Returns how many were applied.
    core::Result<std::size_t> refresh(const core::MetadataIndex& index);
    // Applies one record: a tombstone removes the file, anything else adds or updates it.
    // A record whose path collides with a directory (or runs through a file) is ignored.
    void apply(const core::FileRecord& record);

    std::optional<Ino> lookup(Ino parent, std::string_view name) const;
    std::optional<Ino> resolve(std::string_view path) const;
    std::optional<Attr> getattr(Ino ino) const;
    std::optional<std::vector<DirEntry>> readdir(Ino ino) const;
    // The current version of a file, for pinning at open.
    std::optional<core::FileRecord> file_record(Ino ino) const;

    std::uint64_t applied_sequence() const;

private:
    struct Node {
        Ino parent = 0;
        std::string name;
        NodeType type = NodeType::directory;
        std::map<std::string, Ino, std::less<>> children;
        std::optional<core::FileRecord> record;
        std::int64_t mtime_ns = 0;
    };

    void upsert_locked(const core::FileRecord& record);
    void remove_locked(std::string_view path);
    std::optional<Ino> resolve_locked(std::string_view path) const;
    Attr attr_locked(Ino ino, const Node& node) const;

    mutable std::shared_mutex mutex_;
    std::unordered_map<Ino, Node> nodes_;
    Ino next_ino_ = kRootIno + 1;
    std::uint64_t applied_sequence_ = 0;
};

}  // namespace ghostdrive::vfs
