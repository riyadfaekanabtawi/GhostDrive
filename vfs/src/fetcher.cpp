#include "ghostdrive/vfs/fetcher.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <optional>
#include <string>

#include "async.hpp"
#include "ghostdrive/vfs/chunk_exchange.hpp"

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;
using core::Result;
using core::SharedChunk;

namespace {

// One GetChunk that gives up after `timeout`. The request itself keeps running (it holds the
// connection) and its late answer is discarded; the stream ends when the connection does.
asio::awaitable<Result<core::Bytes>> request_with_deadline(
    asio::any_io_executor io, std::shared_ptr<transport::Connection> conn, core::ChunkId id,
    std::uint32_t size, std::chrono::milliseconds timeout) {
    struct State {
        explicit State(asio::any_io_executor ex) : timer(std::move(ex)) {}
        asio::steady_timer timer;
        std::optional<Result<core::Bytes>> result;
    };
    auto state = std::make_shared<State>(io);
    state->timer.expires_after(timeout);
    asio::co_spawn(
        io,
        [state, conn, id, size]() -> asio::awaitable<void> {
            auto r = co_await request_chunk(*conn, id, size);
            state->result = std::move(r);
            state->timer.cancel();
        },
        asio::detached);
    std::error_code ec;
    co_await state->timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    if (!state->result) co_return Error{Errc::timeout, "peer did not answer GetChunk in time"};
    co_return std::move(*state->result);
}

}  // namespace

Fetcher::Fetcher(asio::any_io_executor io, asio::any_io_executor disk,
                 const core::ChunkStore& store, const core::MetadataIndex& index,
                 core::ChunkCache& cache, ConnectionLookup peers,
                 std::chrono::milliseconds request_timeout, std::chrono::milliseconds holder_wait)
    : io_(std::move(io)),
      disk_(std::move(disk)),
      store_(store),
      index_(index),
      cache_(cache),
      peers_(std::move(peers)),
      request_timeout_(request_timeout),
      holder_wait_(holder_wait) {}

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

void Fetcher::holders_changed() {
    ++holders_generation_;
    for (auto& weak : holder_waiters_) {
        if (auto timer = weak.lock()) timer->cancel();
    }
    holder_waiters_.clear();
}

asio::awaitable<bool> Fetcher::wait_for_holders(std::chrono::steady_clock::time_point deadline,
                                                std::uint64_t seen) {
    if (holders_generation_ != seen) co_return true;
    if (std::chrono::steady_clock::now() >= deadline) co_return false;
    ++stats_.holder_waits;
    auto timer = std::make_shared<asio::steady_timer>(io_, deadline);
    std::erase_if(holder_waiters_, [](const auto& weak) { return weak.expired(); });
    holder_waiters_.push_back(timer);
    std::error_code ec;
    co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
    co_return holders_generation_ != seen;
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
    // A file's record can arrive before the record that names a holder of its chunks, and a
    // holder can be about to connect, so a fetch with no holder to ask waits a while for one.
    const auto deadline = std::chrono::steady_clock::now() + holder_wait_;
    for (;;) {
        const auto seen = holders_generation_;
        auto holders = co_await detail::run_on(disk_, [this, id] { return index_.holders(id); });
        if (!holders) co_return holders.error();

        std::optional<Error> last_error;
        for (const auto& key : holders.value()) {
            auto conn = peers_ ? peers_(key) : nullptr;
            if (!conn || !conn->is_open()) continue;
            auto data = co_await request_with_deadline(io_, conn, id, size, request_timeout_);
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
        if (!co_await wait_for_holders(deadline, seen)) break;
    }
    co_return Error{Errc::not_found, "no reachable device holds chunk " + id.to_hex()};
}

}  // namespace ghostdrive::vfs
