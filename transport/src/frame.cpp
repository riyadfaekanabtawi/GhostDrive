#include "ghostdrive/transport/frame.hpp"

#include <algorithm>
#include <string>

#include "ghostdrive/transport/endpoint.hpp"

namespace ghostdrive::transport::wire {

using core::Errc;
using core::Error;

namespace {

void put_u32(std::span<std::byte> out, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) out[i] = static_cast<std::byte>(value >> (8 * i));
}

std::uint32_t get_u32(std::span<const std::byte> in) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(in[i]) << (8 * i);
    return value;
}

bool known_type(std::uint8_t type) noexcept {
    return type >= static_cast<std::uint8_t>(FrameType::get_chunk) &&
           type <= static_cast<std::uint8_t>(FrameType::discovery_keys);
}

core::Bytes make_frame(FrameType type, std::span<const std::byte> payload) {
    core::Bytes frame(kHeaderSize + payload.size());
    auto header = encode_header(type, static_cast<std::uint32_t>(payload.size()));
    std::copy(header.begin(), header.end(), frame.begin());
    std::copy(payload.begin(), payload.end(), frame.begin() + kHeaderSize);
    return frame;
}

}  // namespace

HeaderBytes encode_header(FrameType type, std::uint32_t length) noexcept {
    HeaderBytes header{};
    put_u32(header, length);
    header[4] = static_cast<std::byte>(kVersion);
    header[5] = static_cast<std::byte>(type);
    return header;
}

core::Result<FrameHeader> decode_header(std::span<const std::byte, kHeaderSize> bytes,
                                        std::uint32_t max_payload) {
    auto version = std::to_integer<std::uint8_t>(bytes[4]);
    if (version != kVersion) {
        return Error{Errc::protocol, "unsupported frame version " + std::to_string(version)};
    }
    auto type = std::to_integer<std::uint8_t>(bytes[5]);
    if (!known_type(type)) {
        return Error{Errc::protocol, "unknown frame type " + std::to_string(type)};
    }
    auto length = get_u32(bytes);
    if (length > max_payload) {
        return Error{Errc::protocol,
                     "frame payload of " + std::to_string(length) + " bytes exceeds the limit"};
    }
    return FrameHeader{static_cast<FrameType>(type), length};
}

core::Bytes encode_get_chunk(const core::ChunkId& id) {
    auto bytes = id.to_bytes();
    return make_frame(FrameType::get_chunk, bytes);
}

core::Result<core::ChunkId> decode_get_chunk(std::span<const std::byte> payload) {
    if (payload.size() != core::kChunkIdSize) {
        return Error{Errc::protocol, "GetChunk payload has the wrong size"};
    }
    auto id = core::ChunkId::from_bytes(payload);
    if (!id) return Error{Errc::protocol, id.error().message};
    return id;
}

core::Bytes encode_error(ErrorCode code) {
    auto value = static_cast<std::uint16_t>(code);
    std::array<std::byte, 2> payload{static_cast<std::byte>(value & 0xFF),
                                     static_cast<std::byte>(value >> 8)};
    return make_frame(FrameType::error, payload);
}

core::Result<ErrorCode> decode_error(std::span<const std::byte> payload) {
    if (payload.size() != 2) return Error{Errc::protocol, "Error payload has the wrong size"};
    auto value = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(payload[0]) |
                                            std::to_integer<std::uint16_t>(payload[1]) << 8);
    // Unknown codes are kept: a newer peer may send codes this version does not name.
    return static_cast<ErrorCode>(value);
}

asio::awaitable<core::Status> read_exact(Stream& stream, std::span<std::byte> out) {
    std::size_t filled = 0;
    while (filled < out.size()) {
        auto n = co_await stream.read(out.subspan(filled));
        if (!n) co_return n.error();
        if (n.value() == 0) co_return Error{Errc::protocol, "stream ended inside a frame"};
        filled += n.value();
    }
    co_return core::Status{};
}

asio::awaitable<core::Result<Frame>> read_frame(Stream& stream, std::uint32_t max_payload) {
    HeaderBytes header_bytes{};
    if (auto st = co_await read_exact(stream, header_bytes); !st) co_return st.error();
    auto header = decode_header(header_bytes, max_payload);
    if (!header) co_return header.error();
    // Grow the buffer only as bytes arrive, so a header that claims a large payload commits no
    // more memory than the peer actually sends (which QUIC flow control bounds).
    constexpr std::size_t kStep = 256 * 1024;
    Frame frame{header.value().type, {}};
    std::size_t remaining = header.value().length;
    while (remaining > 0) {
        std::size_t step = std::min(remaining, kStep);
        std::size_t old = frame.payload.size();
        frame.payload.resize(old + step);
        if (auto st = co_await read_exact(stream, std::span(frame.payload).subspan(old)); !st) {
            co_return st.error();
        }
        remaining -= step;
    }
    co_return frame;
}

}  // namespace ghostdrive::transport::wire
