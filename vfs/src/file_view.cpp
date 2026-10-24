#include "ghostdrive/vfs/file_view.hpp"

#include <algorithm>
#include <string>

#include "ghostdrive/core/chunking.hpp"

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;

namespace {

// Reads arriving this close to where the last one ended still count as sequential. FUSE-T's
// NFS client issues reads in parallel and out of order (ARCHITECTURE.md section 4).
constexpr std::uint64_t kSequentialSlack = 1024 * 1024;

}  // namespace

FileView::FileView(core::FileRecord pinned, Fetcher& fetcher, std::size_t max_readahead_chunks)
    : record_(std::move(pinned)), fetcher_(fetcher), max_readahead_(max_readahead_chunks) {}

std::uint32_t FileView::chunk_size(std::uint64_t index) const noexcept {
    std::uint64_t start = index * record_.block_size;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(record_.block_size, record_.size - start));
}

void FileView::update_readahead(std::uint64_t offset, std::uint64_t last_chunk) {
    bool sequential = has_read_ && offset + kSequentialSlack >= last_end_ &&
                      offset <= last_end_ + kSequentialSlack;
    if (!has_read_ && offset == 0) sequential = true;  // reading from the start
    if (sequential) {
        window_ = std::min(max_readahead_, window_ == 0 ? std::size_t{1} : window_ * 2);
    } else {
        window_ = 0;
    }
    std::uint64_t count = record_.chunks.size();
    for (std::uint64_t i = last_chunk + 1; i <= last_chunk + window_ && i < count; ++i) {
        fetcher_.prefetch(record_.chunks[i], chunk_size(i));
    }
}

asio::awaitable<core::Result<ReadReply>> FileView::read(std::uint64_t offset, std::size_t size) {
    ReadReply reply;
    auto span = core::chunks_for_range(offset, size, record_.size, record_.block_size);
    if (!span) co_return reply;  // at or past the end, or an empty read

    update_readahead(offset, span->last);
    std::uint64_t end = std::min<std::uint64_t>(offset + size, record_.size);
    has_read_ = true;
    last_end_ = end;

    for (std::uint64_t i = span->first; i <= span->last; ++i) {
        if (i >= record_.chunks.size()) {
            co_return Error{Errc::corrupt, "file record has fewer chunks than its size needs"};
        }
        auto expected = chunk_size(i);
        auto chunk = co_await fetcher_.get(record_.chunks[i], expected);
        if (!chunk) co_return chunk.error();
        if (chunk.value()->size() != expected) {
            co_return Error{Errc::corrupt, "chunk " + std::to_string(i) + " has the wrong size"};
        }
        std::uint64_t chunk_start = i * record_.block_size;
        std::uint64_t from = std::max(offset, chunk_start) - chunk_start;
        std::uint64_t to = std::min<std::uint64_t>(end - chunk_start, expected);
        reply.slices.push_back(
            std::span<const std::byte>(*chunk.value())
                .subspan(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)));
        reply.chunks.push_back(std::move(chunk).value());
    }
    co_return reply;
}

}  // namespace ghostdrive::vfs
