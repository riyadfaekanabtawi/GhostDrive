#pragma once

#include <asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ghostdrive/core/chunk_cache.hpp"
#include "ghostdrive/core/file_record.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/vfs/fetcher.hpp"

namespace ghostdrive::vfs {

// The bytes of one read, as slices of chunk buffers. `chunks` keeps the buffers alive until
// the reply has been sent, so nothing is copied before the FUSE reply.
struct ReadReply {
    std::vector<core::SharedChunk> chunks;
    std::vector<std::span<const std::byte>> slices;

    std::size_t size() const noexcept {
        std::size_t total = 0;
        for (auto s : slices) total += s.size();
        return total;
    }
};

// A file version pinned at open: a remote edit while it is open cannot mix versions. Use from
// the fetcher's executor only.
class FileView {
public:
    FileView(core::FileRecord pinned, Fetcher& fetcher, std::size_t max_readahead_chunks);

    // Returns up to `size` bytes at `offset`; an empty reply at or past the end of the file.
    asio::awaitable<core::Result<ReadReply>> read(std::uint64_t offset, std::size_t size);

    const core::FileRecord& record() const noexcept { return record_; }
    std::size_t readahead_window() const noexcept { return window_; }

private:
    std::uint32_t chunk_size(std::uint64_t index) const noexcept;
    void update_readahead(std::uint64_t offset, std::uint64_t last_chunk);

    core::FileRecord record_;
    Fetcher& fetcher_;
    std::size_t max_readahead_;
    std::size_t window_ = 0;
    std::uint64_t last_end_ = 0;
    bool has_read_ = false;
};

}  // namespace ghostdrive::vfs
