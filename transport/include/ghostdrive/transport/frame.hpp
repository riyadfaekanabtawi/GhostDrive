#pragma once

#include <array>
#include <asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/result.hpp"

namespace ghostdrive::transport {

class Stream;

// Peer protocol frames. Every frame is
//   u32 payload length (little endian) | u8 version | u8 type | payload
// A stream's first frame says what it is for: GetChunk (answered by ChunkData or Error), Hello
// (an index exchange), or Join (pairing).
namespace wire {

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 6;
// Largest payload accepted: a 16 MiB chunk (the maximum block size) is the biggest frame.
inline constexpr std::uint32_t kMaxPayload = 16u * 1024 * 1024;

enum class FrameType : std::uint8_t {
    get_chunk = 1,
    chunk_data = 2,
    error = 3,
    // Index exchange (one stream per direction, opened by the sender of the records).
    hello = 4,
    index_request = 5,
    index_batch = 6,
    // Pairing a new device by invite code.
    join = 7,
    drive_info = 8,
    // Rendezvous server control stream.
    rendezvous_register = 9,
    rendezvous_registered = 10,
    rendezvous_connect = 11,
    rendezvous_peer_at = 12,
};

enum class ErrorCode : std::uint16_t {
    not_found = 1,
    internal = 2,
    bad_request = 3,
};

struct FrameHeader {
    FrameType type;
    std::uint32_t length;
};

using HeaderBytes = std::array<std::byte, kHeaderSize>;

HeaderBytes encode_header(FrameType type, std::uint32_t length) noexcept;
// Rejects unknown versions and types, and payloads over max_payload.
core::Result<FrameHeader> decode_header(std::span<const std::byte, kHeaderSize> bytes,
                                        std::uint32_t max_payload = kMaxPayload);

core::Bytes encode_get_chunk(const core::ChunkId& id);
core::Result<core::ChunkId> decode_get_chunk(std::span<const std::byte> payload);

core::Bytes encode_error(ErrorCode code);
core::Result<ErrorCode> decode_error(std::span<const std::byte> payload);

// Reads exactly out.size() bytes; a stream that ends early is a protocol error.
asio::awaitable<core::Status> read_exact(Stream& stream, std::span<std::byte> out);

struct Frame {
    FrameType type;
    core::Bytes payload;
};
asio::awaitable<core::Result<Frame>> read_frame(Stream& stream,
                                                std::uint32_t max_payload = kMaxPayload);

}  // namespace wire
}  // namespace ghostdrive::transport
