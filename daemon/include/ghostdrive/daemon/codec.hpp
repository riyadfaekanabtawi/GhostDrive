#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/file_record.hpp"
#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"

// Payloads of the daemon's peer messages, framed with transport::wire. Encoders return whole
// frames; decoders take the payload and reject anything malformed or over the limits below, so
// a peer cannot make us allocate without bound. Device names come back with control bytes
// replaced, since they are shown in terminals.
namespace ghostdrive::daemon {

inline constexpr std::size_t kMaxPathBytes = 4096;
inline constexpr std::size_t kMaxNameBytes = 255;
inline constexpr std::size_t kMaxChunksPerRecord = 1u << 20;
inline constexpr std::size_t kMaxBatchEntries = 1024;
inline constexpr std::size_t kMaxMembers = 256;
inline constexpr std::size_t kTokenSize = 16;
inline constexpr std::size_t kDriveIdSize = 32;

using Token = std::array<std::byte, kTokenSize>;
using DriveId = std::array<std::byte, kDriveIdSize>;

// Index exchange. The device sending records opens a stream with Hello; the receiver answers
// with IndexRequest saying where to resume; IndexBatch frames follow for as long as the
// connection lives.
struct Hello {
    std::uint64_t index_id = 0;
    std::uint64_t max_sequence = 0;
};

struct IndexRequest {
    std::uint64_t after = 0;  // send records with a sequence greater than this
};

struct IndexEntry {
    core::FileRecord record;    // `sequence` is the sender's
    bool sender_holds = false;  // the sender has every chunk of this version locally
};

// Pairing. A new device proves it holds the invite token over a connection where it pinned the
// inviter's key; the inviter answers with the drive's identity and members.
struct Join {
    Token token{};
    std::string name;
    std::uint16_t listen_port = 0;
};

struct Member {
    core::PublicKey key{};
    std::string name;
    std::string address;  // "host:port", empty when unknown
};

struct DriveInfo {
    DriveId drive_id{};
    core::DriveSecret secret{};
    std::vector<Member> members;
};

core::Bytes encode_hello(const Hello& hello);
core::Result<Hello> decode_hello(std::span<const std::byte> payload);

core::Bytes encode_index_request(const IndexRequest& request);
core::Result<IndexRequest> decode_index_request(std::span<const std::byte> payload);

core::Bytes encode_index_batch(std::span<const IndexEntry> entries);
core::Result<std::vector<IndexEntry>> decode_index_batch(std::span<const std::byte> payload);

core::Bytes encode_join(const Join& join);
core::Result<Join> decode_join(std::span<const std::byte> payload);

core::Bytes encode_drive_info(const DriveInfo& info);
core::Result<DriveInfo> decode_drive_info(std::span<const std::byte> payload);

}  // namespace ghostdrive::daemon
