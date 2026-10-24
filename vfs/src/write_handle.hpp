#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ghostdrive/core/chunk_store.hpp"
#include "ghostdrive/core/file_record.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "ghostdrive/core/metadata_index.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/vfs/fetcher.hpp"
#include "ghostdrive/vfs/file_view.hpp"

namespace ghostdrive::vfs {

// One open-for-write file. Written bytes go to a sparse staging file at their real offsets; a
// file's block size stays fixed once chosen, so only the blocks a write touched are rehashed
// at commit and every untouched block keeps its chunk ID. A partial write to a block that has
// not been touched yet first copies that block in (fetching it if it is remote).
//
// Coroutines run on the io executor, one at a time per handle (Filesystem serializes them);
// disk work runs on the disk executor. size() may be read from any thread.
class WriteHandle {
public:
    struct Context {
        core::ChunkStore& store;
        core::MetadataIndex& index;
        core::HybridClock& clock;
        Fetcher& fetcher;
        asio::any_io_executor disk;
        // Held while committing and while the path changes, shared with the Filesystem's
        // other mutations.
        std::mutex& mutation_mutex;
        // Called with the mutation mutex held, after each commit reaches the index.
        std::function<void()> after_commit;
    };

    // `base` is the version being edited, or nullopt for a file that has never been committed.
    static core::Result<std::shared_ptr<WriteHandle>> open(Context context, std::string path,
                                                           std::optional<core::FileRecord> base,
                                                           std::uint32_t new_file_mode,
                                                           std::filesystem::path staging_file,
                                                           bool truncate);
    WriteHandle(const WriteHandle&) = delete;
    WriteHandle& operator=(const WriteHandle&) = delete;
    ~WriteHandle();

    asio::awaitable<core::Status> write(std::uint64_t offset, core::Bytes data);
    asio::awaitable<core::Status> truncate(std::uint64_t size);
    asio::awaitable<core::Result<ReadReply>> read(std::uint64_t offset, std::size_t size);
    // Chunks the dirty blocks into the store and commits a new version. A no-op when clean.
    asio::awaitable<core::Status> commit();

    void set_mode(std::uint32_t perm);
    void set_mtime(std::int64_t mtime_ns);
    // Call with the mutation mutex held.
    void set_path(std::string path) { path_ = std::move(path); }
    const std::string& path() const { return path_; }
    // The file was unlinked: further writes still work, but nothing is ever committed.
    void orphan() { orphaned_ = true; }

    std::uint64_t size() const noexcept { return size_.load(); }
    bool dirty() const noexcept { return dirty_any_; }

    struct Private {};
    WriteHandle(Private, Context context, std::string path, std::optional<core::FileRecord> base,
                std::uint32_t new_file_mode, std::filesystem::path staging_file, int fd);

private:
    bool from_base(std::uint64_t block) const noexcept;
    void mark_dirty(std::uint64_t block);
    std::uint32_t base_chunk_size(std::uint64_t block) const noexcept;
    asio::awaitable<core::Status> copy_up(std::uint64_t block);
    asio::awaitable<core::Status> resize(std::uint64_t size);

    Context ctx_;
    std::string path_;
    std::optional<core::FileRecord> base_;
    std::uint32_t new_file_mode_;
    std::filesystem::path staging_file_;
    int fd_;

    std::uint32_t block_ = 0;       // 0 until a block size is chosen (new or emptied file)
    std::uint64_t valid_base_ = 0;  // blocks below this may still come from base_
    std::vector<bool> dirty_;
    bool dirty_any_ = false;
    std::atomic<std::uint64_t> size_{0};
    std::optional<std::uint32_t> mode_override_;
    std::optional<std::int64_t> mtime_override_;
    std::atomic<bool> orphaned_{false};
};

}  // namespace ghostdrive::vfs
