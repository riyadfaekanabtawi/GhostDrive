#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ghostdrive/core/chunk_store.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "ghostdrive/core/metadata_index.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/vfs/fetcher.hpp"
#include "ghostdrive/vfs/file_view.hpp"
#include "ghostdrive/vfs/tree.hpp"

namespace ghostdrive::vfs {

class WriteHandle;

struct FilesystemOptions {
    // A read that has not completed by then fails with Errc::timeout; the chunk fetch keeps
    // going so the cache still benefits.
    std::chrono::milliseconds request_timeout{30'000};
    std::size_t max_readahead_chunks = 8;
};

// What writes need. Without it the filesystem is read-only.
struct WriteSupport {
    core::ChunkStore& store;
    core::MetadataIndex& index;
    core::HybridClock& clock;
    asio::any_io_executor disk;
    // Staging files for open writers; must be on the same device as the chunk store.
    std::filesystem::path staging_dir;
};

struct SetAttr {
    std::optional<std::uint64_t> size;
    std::optional<std::uint32_t> perm;
    std::optional<std::int64_t> mtime_ns;
};

// Backend-independent filesystem operations for the FUSE adapters. Metadata calls are
// synchronous and thread-safe. Reads, writes and commits run on the io executor and complete
// through callbacks there, so a FUSE thread never blocks on the network. Operations on one
// write handle run in the order they were issued.
//
// Writes follow ARCHITECTURE.md section 3.3: bytes go to a staging file, and the new version
// is committed to the index on flush, fsync, or the last release.
class Filesystem {
public:
    using ReadCallback = std::function<void(core::Result<ReadReply>)>;
    using StatusCallback = std::function<void(core::Status)>;
    using AttrCallback = std::function<void(core::Result<Attr>)>;

    Filesystem(asio::any_io_executor io, Tree& tree, Fetcher& fetcher,
               FilesystemOptions options = {}, std::optional<WriteSupport> writes = std::nullopt);
    ~Filesystem();

    bool writable() const noexcept { return writes_.has_value(); }

    std::optional<Ino> lookup(Ino parent, std::string_view name) const {
        return tree_.lookup(parent, name);
    }
    // While a file is open for writing, its size is the size being written.
    std::optional<Attr> getattr(Ino ino) const;
    std::optional<std::vector<DirEntry>> readdir(Ino ino) const { return tree_.readdir(ino); }

    // Pins the file's current version and returns a handle. Fails with not_found for missing
    // inodes and is_directory for directories; `write` needs WriteSupport.
    core::Result<std::uint64_t> open(Ino ino, bool write = false, bool truncate = false);
    // Creates an empty file, visible at once, and opens it for writing.
    core::Result<std::pair<Attr, std::uint64_t>> create(Ino parent, std::string_view name,
                                                        std::uint32_t perm);
    core::Result<Attr> mkdir(Ino parent, std::string_view name, std::uint32_t perm);
    core::Status unlink(Ino parent, std::string_view name);
    core::Status rmdir(Ino parent, std::string_view name);
    // Replaces an existing file at the target; a directory target must be empty. Inodes are
    // kept, so open handles follow the rename.
    core::Status rename(Ino parent, std::string_view name, Ino new_parent,
                        std::string_view new_name, bool no_replace = false);

    // Callbacks run exactly once, on the io executor.
    void read(std::uint64_t handle, std::uint64_t offset, std::size_t size, ReadCallback done);
    void write(std::uint64_t handle, std::uint64_t offset, core::Bytes data, StatusCallback done);
    // Commits pending writes (close and fsync).
    void flush(std::uint64_t handle, StatusCallback done);
    // `handle` is the open file the change came through, if any.
    void setattr(Ino ino, SetAttr changes, std::optional<std::uint64_t> handle, AttrCallback done);
    // Drops the handle; pending writes are committed in the background.
    void release(std::uint64_t handle);

    // Applies remote index changes. Serialized with local mutations.
    core::Result<std::size_t> refresh(const core::MetadataIndex& index);

    std::size_t open_handles() const;

private:
    struct Handle;
    using Op = std::function<asio::awaitable<void>()>;

    core::Status require_writable() const;
    core::Result<std::string> child_path(Ino parent, std::string_view name) const;
    std::shared_ptr<Handle> find(std::uint64_t handle) const;
    void enqueue(std::shared_ptr<Handle> handle, Op op);
    // Live handles open for writing on `ino`, newest first.
    std::vector<std::shared_ptr<Handle>> writers_for(Ino ino) const;
    // A write handle registered for `ino`; the caller decides whether it gets a handle number.
    core::Result<std::shared_ptr<Handle>> make_writer(Ino ino, std::string path,
                                                      std::optional<core::FileRecord> base,
                                                      std::uint32_t perm, bool truncate);
    core::Status commit_record_locked(core::FileRecord record);
    void refresh_locked();

    asio::any_io_executor io_;
    Tree& tree_;
    Fetcher& fetcher_;
    FilesystemOptions options_;
    std::optional<WriteSupport> writes_;

    // Serializes index mutations, the tree refresh that follows them, and writer paths.
    std::mutex mutation_mutex_;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Handle>> handles_;
    std::unordered_map<Ino, std::vector<std::weak_ptr<Handle>>> writers_;
    std::uint64_t next_handle_ = 1;
    std::uint64_t next_staging_ = 1;
};

// errno for an error reported to the kernel.
int to_errno(const core::Error& error) noexcept;

}  // namespace ghostdrive::vfs
