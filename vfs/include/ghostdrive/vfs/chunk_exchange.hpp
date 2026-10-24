#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <cstdint>
#include <memory>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/chunk_store.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/transport/endpoint.hpp"
#include "ghostdrive/transport/frame.hpp"

namespace ghostdrive::vfs {

// Answers GetChunk requests on every stream the peer opens, until the connection closes. Disk
// reads run on `disk`. The store must outlive the connection's streams.
asio::awaitable<void> serve_chunks(std::shared_ptr<transport::Connection> conn,
                                   const core::ChunkStore& store, asio::any_io_executor disk);

// Answers one GetChunk whose request frame the caller already read from `stream`.
asio::awaitable<void> answer_chunk_request(std::shared_ptr<transport::Stream> stream,
                                           const transport::wire::Frame& request,
                                           const core::ChunkStore& store,
                                           asio::any_io_executor disk);

// Sends one GetChunk and returns the bytes as received. They are NOT verified yet: callers must
// check the hash before using or caching them. Replies over max_size are rejected.
asio::awaitable<core::Result<core::Bytes>> request_chunk(transport::Connection& conn,
                                                         core::ChunkId id,
                                                         std::uint32_t max_size);

}  // namespace ghostdrive::vfs
