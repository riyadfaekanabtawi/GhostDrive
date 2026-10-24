#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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
// Membership bounds (membership.hpp): devices that are members or in doubt at once, keys known
// that are not removed for good, entries in all, this device's own entries, entries per other
// author, and keys removed for good. Names in entries are short so the log stays small.
inline constexpr std::size_t kMaxDevices = 64;
inline constexpr std::size_t kMaxKnownKeys = 256;
inline constexpr std::size_t kMaxMembershipEntries = 32768;
inline constexpr std::size_t kMaxOwnEntries = 2 * kMaxKnownKeys;
// Only authors that are not out keep entries, at most kMaxKnownKeys of them, so all of theirs
// fit beside this device's own.
inline constexpr std::size_t kMaxEntriesPerAuthor =
    (kMaxMembershipEntries - kMaxOwnEntries) / kMaxKnownKeys;
inline constexpr std::size_t kMaxEverRemoved = kMaxMembershipEntries + kMaxKnownKeys;
inline constexpr std::size_t kMaxMemberNameBytes = 64;
inline constexpr std::size_t kMaxMembershipBatch = 256;

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

// A signed statement by a member: the subject belongs to the drive (add), or has been taken
// out of it (remove). How entries combine into a membership is in membership.hpp.
struct MembershipEntry {
    enum class Kind : std::uint8_t { add = 1, remove = 2 };
    Kind kind = Kind::add;
    bool invited = false;  // an add made while pairing: the author invited the subject
    core::PublicKey author{};
    core::PublicKey subject{};
    std::int64_t time_ms = 0;  // the author's clock; shown to users, never used for ordering
    std::string name;          // add only: the subject's device name
    core::Signature signature{};

    bool operator==(const MembershipEntry&) const = default;
};

struct DriveInfo {
    DriveId drive_id{};
    core::DriveSecret secret{};
    std::vector<Member> members;
    std::string rendezvous;  // "host:port FINGERPRINT", empty when the drive has none
    std::vector<MembershipEntry> membership;
};

// A device name made safe to show in a terminal: invalid UTF-8, control characters (C0 and
// C1) and bidirectional overrides become '?'.
std::string printable_name(std::string_view name);

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

// The bytes an entry's signature covers. They include the drive ID, so an entry cannot be
// replayed into another drive.
core::Bytes membership_signed_bytes(const DriveId& drive, const MembershipEntry& entry);
// Decoding checks the form of each entry, not its signature.
core::Bytes encode_membership(std::span<const MembershipEntry> entries);
core::Bytes encode_membership_payload(std::span<const MembershipEntry> entries);
core::Result<std::vector<MembershipEntry>> decode_membership(std::span<const std::byte> payload);

}  // namespace ghostdrive::daemon
