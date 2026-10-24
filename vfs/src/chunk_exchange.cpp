#include "ghostdrive/vfs/chunk_exchange.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/this_coro.hpp>
#include <string>

#include "async.hpp"
#include "ghostdrive/transport/frame.hpp"

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;
namespace wire = transport::wire;

namespace {

// A GetChunk request is a single chunk ID; anything larger is not a request we understand.
constexpr std::uint32_t kMaxRequestPayload = 64;

asio::awaitable<void> serve_one(std::shared_ptr<transport::Stream> stream,
                                const core::ChunkStore& store, asio::any_io_executor disk) {
    auto request = co_await wire::read_frame(*stream, kMaxRequestPayload);
    if (!request) co_return;
    co_await answer_chunk_request(std::move(stream), request.value(), store, std::move(disk));
}

}  // namespace

asio::awaitable<void> answer_chunk_request(std::shared_ptr<transport::Stream> stream,
                                           const wire::Frame& request,
                                           const core::ChunkStore& store,
                                           asio::any_io_executor disk) {
    std::optional<core::ChunkId> id;
    if (request.type == wire::FrameType::get_chunk) {
        auto decoded = wire::decode_get_chunk(request.payload);
        if (decoded) id = decoded.value();
    }
    if (!id) {
        auto reply = wire::encode_error(wire::ErrorCode::bad_request);
        (void)co_await stream->write(reply, true);
        co_return;
    }

    // Re-verifying on read means a corrupted local file is never served as good data.
    auto data =
        co_await detail::run_on(disk, [&store, id = *id] { return store.read_verified(id); });
    if (!data) {
        auto code = data.error().code == Errc::not_found ? wire::ErrorCode::not_found
                                                         : wire::ErrorCode::internal;
        auto reply = wire::encode_error(code);
        (void)co_await stream->write(reply, true);
        co_return;
    }
    auto header = wire::encode_header(wire::FrameType::chunk_data,
                                      static_cast<std::uint32_t>(data.value().size()));
    const std::span<const std::byte> parts[] = {header, data.value()};
    (void)co_await stream->write(parts, true);
}


asio::awaitable<void> serve_chunks(std::shared_ptr<transport::Connection> conn,
                                   const core::ChunkStore& store, asio::any_io_executor disk) {
    auto executor = co_await asio::this_coro::executor;
    for (;;) {
        auto stream = co_await conn->accept_stream();
        if (!stream) co_return;  // connection closed
        asio::co_spawn(executor, serve_one(std::move(stream).value(), store, disk), asio::detached);
    }
}

asio::awaitable<core::Result<core::Bytes>> request_chunk(transport::Connection& conn,
                                                         core::ChunkId id,
                                                         std::uint32_t max_size) {
    auto stream = co_await conn.open_stream();
    if (!stream) co_return stream.error();
    auto request = wire::encode_get_chunk(id);
    if (auto st = co_await stream.value()->write(request, true); !st) co_return st.error();

    auto limit = std::clamp<std::uint32_t>(max_size, 2, wire::kMaxPayload);
    auto reply = co_await wire::read_frame(*stream.value(), limit);
    if (!reply) co_return reply.error();
    switch (reply.value().type) {
        case wire::FrameType::chunk_data:
            co_return std::move(reply.value().payload);
        case wire::FrameType::error: {
            auto code = wire::decode_error(reply.value().payload);
            if (!code) co_return code.error();
            if (code.value() == wire::ErrorCode::not_found) {
                co_return Error{Errc::not_found, "peer does not hold chunk " + id.to_hex()};
            }
            co_return Error{Errc::io, "peer failed to serve chunk " + id.to_hex() + " (code " +
                                          std::to_string(static_cast<unsigned>(code.value())) +
                                          ")"};
        }
        default:
            co_return Error{Errc::protocol, "unexpected reply to GetChunk"};
    }
}

}  // namespace ghostdrive::vfs
