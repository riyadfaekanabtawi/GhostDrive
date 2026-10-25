#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/file_record.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"

namespace ghostdrive::core {

struct DeviceRecord {
    PublicKey key{};
    std::uint64_t short_id = 0;
    std::string name;
    // Resume point for index exchange with this peer (Syncthing BEP index_id / max sequence).
    std::uint64_t peer_index_id = 0;
    std::uint64_t peer_max_sequence = 0;
};

struct ApplyOutcome {
    enum class Kind {
        applied,     // remote version replaced the local one
        ignored,     // local already has this version or a newer one
        kept_local,  // concurrent; local version won and absorbed the remote history
        conflict,    // concurrent edits; the loser was kept as a conflict copy
    };
    Kind kind;
    std::string conflict_path;  // set when kind == conflict
};

// Replicated metadata index backed by SQLite (WAL). Every change, local or remote, gets the next
// local sequence number so peers can fetch only what changed since their last exchange.
// Local changes are signed with the device key; remote ones must carry a valid signature by the
// device in their modified_by. Thread-safe: calls are serialized internally.
class MetadataIndex {
public:
    // `key` signs local changes and must outlive the index. The local device is its short ID.
    static Result<MetadataIndex> open(const std::filesystem::path& db_path, const DriveId& drive,
                                      const DeviceKey& key);

    MetadataIndex(MetadataIndex&&) noexcept;
    MetadataIndex& operator=(MetadataIndex&&) noexcept;
    ~MetadataIndex();

    std::uint64_t local_device() const noexcept;
    std::uint64_t index_id() const noexcept;
    Result<std::uint64_t> max_sequence() const;

    Result<std::optional<FileRecord>> get(std::string_view path) const;
    // Records with sequence > after, in sequence order, tombstones included. signed_only leaves
    // out records stored unsigned before records were signed; only those may be sent to peers.
    Result<std::vector<FileRecord>> records_since(std::uint64_t after, std::size_t limit,
                                                  bool signed_only = false) const;

    // Records a change made on this device. Version, hlc, modified_by and sequence are assigned
    // here; the caller fills content fields.
    Result<FileRecord> commit_local(FileRecord record, HybridClock& clock);
    Result<FileRecord> delete_local(std::string_view path, HybridClock& clock);
    // Rejects with invalid_argument a record that is malformed or whose signature is not valid
    // here, and with not_found one that may become valid once a device is pinned or retired:
    // its signer is unknown, or it is an adopted record whose author or signer is not known as
    // such yet. A record signed by someone other than its author is valid only when the author
    // is retired and the signer is pinned.
    Result<ApplyOutcome> apply_remote(const FileRecord& record, HybridClock& clock);

    // Pinned peers. Only pinned keys may connect; see docs/ARCHITECTURE.md section 7.
    // Pinning also makes the key a signer, for good: a short ID stays bound to its first key.
    Status add_device(const PublicKey& key, std::string_view name);
    Status remove_device(const PublicKey& key);
    // Stops accepting records signed by `key` (a device removed from the drive for good) and
    // adopts the verified records it signed: this device signs them as they are, so devices
    // that never knew the key can still verify them. Retiring is permanent, and retiring a key
    // already retired does nothing. Returns how many records were adopted.
    Result<std::size_t> retire_signer(const PublicKey& key);
    // The device's key is known and not retired.
    bool is_active_signer(std::uint64_t short_id) const;
    bool is_retired_signer(std::uint64_t short_id) const;
    Result<bool> is_pinned(const PublicKey& key) const;
    Result<std::vector<DeviceRecord>> devices() const;
    Status set_peer_progress(const PublicKey& key, std::uint64_t peer_index_id,
                             std::uint64_t peer_max_sequence);

    // Which remote devices hold a chunk locally (the local device is answered by ChunkStore).
    Status set_holds(const PublicKey& device, const ChunkId& chunk, bool holds);
    Result<std::vector<PublicKey>> holders(const ChunkId& chunk) const;

private:
    struct Impl;
    explicit MetadataIndex(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace ghostdrive::core
