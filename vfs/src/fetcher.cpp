#include "ghostdrive/vfs/fetcher.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <optional>
#include <string>

#include "async.hpp"
#include "ghostdrive/vfs/chunk_exchange.hpp"

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;
using core::Result;
using core::SharedChunk;

Fetcher::Fetcher(asio::any_io_executor io, asio::any_io_executor disk,
                 const core::ChunkStore& store, const core::MetadataIndex& index,
                 core::ChunkCache& cache, ConnectionLookup peers)
    : io_(std::move(io)),
      disk_(std::move(disk)),
      store_(store),
      index_(index),
      cache_(cache),
      peers_(std::move(peers)) {}

asio::awaitable<Result<SharedChunk>> Fetcher::get(core::ChunkId id, std::uint32_t size) {
    if (auto cached = cache_.get(id)) {
        ++stats_.cache_hits;
        co_return cached;
    }
    if (auto it = in_flight_.find(id); it != in_flight_.end()) {
        ++stats_.coalesced;
        auto pending = it->second;
        co_return co_await asio::async_initiate<const asio::use_awaitable_t<>,
                                                void(Result<SharedChunk>)>(
            [pending](auto handler) { pending->waiters.emplace_back(std::move(handler)); },
            asio::use_awaitable);
    }

    auto pending = std::make_shared<Pending>();
    in_flight_.emplace(id, pending);
    auto result = co_await load(id, size);
    if (result) cache_.put(id, result.value());
    in_flight_.erase(id);
    for (auto& waiter : pending->waiters) detail::post_completion(io_, waiter, result);
    co_return result;
}

void Fetcher::prefetch(const core::ChunkId& id, std::uint32_t size) {
    if (in_flight_.contains(id) || cache_.contains(id)) return;
    asio::co_spawn(
        io_, [this, id, size]() -> asio::awaitable<void> { (void)co_await get(id, size); },
        asio::detached);
}

asio::awaitable<Result<SharedChunk>> Fetcher::load(core::ChunkId id, std::uint32_t size) {
    auto local = co_await detail::run_on(disk_, [this, id]() -> std::optional<Result<core::Bytes>> {
        if (!store_.contains(id)) return std::nullopt;
        return store_.read_verified(id);
    });
    if (local) {
        if (local->ok() && local->value().size() == size) {
            ++stats_.local_reads;
            co_return std::make_shared<const core::Bytes>(std::move(*local).value());
        }
        // A corrupt or mis-sized local copy is not fatal: a peer may still hold a good one.
    }
    co_return co_await fetch_remote(id, size);
}

asio::awaitable<Result<SharedChunk>> Fetcher::fetch_remote(core::ChunkId id,
                                                           std::uint32_t size) {
    auto holders = co_await detail::run_on(disk_, [this, id] { return index_.holders(id); });
    if (!holders) co_return holders.error();

    std::optional<Error> last_error;
    for (const auto& key : holders.value()) {
        auto conn = peers_ ? peers_(key) : nullptr;
        if (!conn || !conn->is_open()) continue;
        auto data = co_await request_chunk(*conn, id, size);
        if (!data) {
            last_error = data.error();
            continue;
        }
        if (data.value().size() != size) {
            ++stats_.hash_mismatches;
            last_error = Error{Errc::corrupt, "peer sent a chunk of the wrong size"};
            continue;
        }
        auto shared = std::make_shared<const core::Bytes>(std::move(data).value());
        auto matches = co_await detail::run_on(
            disk_, [this, shared, id] { return store_.hasher().hash(*shared) == id; });
        if (!matches) {
            ++stats_.hash_mismatches;
            last_error = Error{Errc::corrupt, "peer sent a chunk that does not match its hash"};
            continue;
        }
        ++stats_.remote_fetches;
        co_return shared;
    }
    if (last_error) co_return *last_error;
    co_return Error{Errc::not_found, "no reachable device holds chunk " + id.to_hex()};
}

}  // namespace ghostdrive::vfs
