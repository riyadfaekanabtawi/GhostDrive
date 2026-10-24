#pragma once

#include <asio/any_completion_handler.hpp>
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ghostdrive/core/chunk_cache.hpp"
#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/chunk_store.hpp"
#include "ghostdrive/core/metadata_index.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/transport/endpoint.hpp"

namespace ghostdrive::vfs {

// Returns a live connection to a device, or null when there is none.
using ConnectionLookup =
    std::function<std::shared_ptr<transport::Connection>(const core::PublicKey&)>;

struct FetcherStats {
    std::uint64_t cache_hits = 0;
    std::uint64_t local_reads = 0;
    std::uint64_t remote_fetches = 0;
    std::uint64_t hash_mismatches = 0;
    std::uint64_t coalesced = 0;
};

// Resolves a chunk ID to verified bytes: cache, then the local store, then each device the
// index lists as a holder. Every byte from a peer is hash-checked before it is returned or
// cached; a peer that sends wrong bytes is skipped for that chunk. Concurrent requests for the
// same chunk share one fetch. Runs on `io`; disk reads and hashing run on `disk`.
class Fetcher {
public:
    Fetcher(asio::any_io_executor io, asio::any_io_executor disk, const core::ChunkStore& store,
            const core::MetadataIndex& index, core::ChunkCache& cache, ConnectionLookup peers);

    // `size` is the chunk's expected length (from the file record); replies of any other
    // length are rejected.
    asio::awaitable<core::Result<core::SharedChunk>> get(core::ChunkId id,
                                                         std::uint32_t size);
    // Starts a fetch in the background unless the chunk is cached or already being fetched.
    void prefetch(const core::ChunkId& id, std::uint32_t size);
    bool in_flight(const core::ChunkId& id) const { return in_flight_.contains(id); }

    const FetcherStats& stats() const noexcept { return stats_; }

private:
    using Waiter = asio::any_completion_handler<void(core::Result<core::SharedChunk>)>;
    struct Pending {
        std::vector<Waiter> waiters;
    };

    asio::awaitable<core::Result<core::SharedChunk>> load(core::ChunkId id,
                                                          std::uint32_t size);
    asio::awaitable<core::Result<core::SharedChunk>> fetch_remote(core::ChunkId id,
                                                                  std::uint32_t size);

    asio::any_io_executor io_;
    asio::any_io_executor disk_;
    const core::ChunkStore& store_;
    const core::MetadataIndex& index_;
    core::ChunkCache& cache_;
    ConnectionLookup peers_;
    std::unordered_map<core::ChunkId, std::shared_ptr<Pending>> in_flight_;
    FetcherStats stats_;
};

}  // namespace ghostdrive::vfs
