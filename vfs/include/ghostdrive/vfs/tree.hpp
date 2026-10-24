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

// File type bits in FileRecord::mode, with POSIX values. A record whose type is a directory
// keeps an otherwise empty directory alive; plain files leave the type bits zero or regular.
inline constexpr std::uint32_t kModeTypeMask = 0170000;
inline constexpr std::uint32_t kModeDirectory = 0040000;

inline bool is_directory_record(const core::FileRecord& record) noexcept {
    return (record.mode & kModeTypeMask) == kModeDirectory;
}

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

// Inode table over the metadata index. Directories come from directory records or are implied
// by file paths; implied ones disappear with their last entry. Inode numbers are never reused,
// so a stale inode held by the kernel can only resolve to nothing, never to a different file.
// Thread-safe.
class Tree {
public:
    Tree();

    // Applies index records newer than the last refresh. Returns how many were applied.
    core::Result<std::size_t> refresh(const core::MetadataIndex& index);
    // Applies one record: a tombstone removes the entry, anything else adds or updates it.
    // A record whose path collides with an entry of the other type, or runs through a file, is
    // ignored.
    void apply(const core::FileRecord& record);

    // Moves an entry, keeping its inode (and its children's) so open handles stay valid. An
    // existing file at `to` is replaced. Fails if `from` is missing, `to` is an existing
    // directory, or `to` runs through a file.
    bool move(std::string_view from, std::string_view to);

    std::optional<Ino> lookup(Ino parent, std::string_view name) const;
    std::optional<Ino> resolve(std::string_view path) const;
    std::optional<std::string> path_of(Ino ino) const;
    std::optional<Attr> getattr(Ino ino) const;
    std::optional<std::vector<DirEntry>> readdir(Ino ino) const;
    // The current version of a file (or directory record), for pinning at open.
    std::optional<core::FileRecord> file_record(Ino ino) const;
    // Paths of every record at or below `ino`: files and directory records.
    std::vector<std::string> record_paths_under(Ino ino) const;

    std::uint64_t applied_sequence() const;

private:
    struct Node {
        Ino parent = 0;
        std::string name;
        NodeType type = NodeType::directory;
        std::map<std::string, Ino, std::less<>> children;
        std::optional<core::FileRecord> record;  // files always; directories when explicit
        std::int64_t mtime_ns = 0;
    };

    void upsert_locked(const core::FileRecord& record);
    void remove_locked(std::string_view path);
    void erase_subtree_locked(Ino ino);
    void prune_locked(Ino dir);
    // Walks to the parent directory of `path`, creating implied directories when `create`.
    std::optional<Ino> parent_for_locked(std::string_view path, bool create);
    std::optional<Ino> resolve_locked(std::string_view path) const;
    std::string path_locked(Ino ino) const;
    Attr attr_locked(Ino ino, const Node& node) const;
    void touch_ancestors_locked(Ino dir, std::int64_t mtime_ns);

    mutable std::shared_mutex mutex_;
    std::unordered_map<Ino, Node> nodes_;
    Ino next_ino_ = kRootIno + 1;
    std::uint64_t applied_sequence_ = 0;
};

}  // namespace ghostdrive::vfs
