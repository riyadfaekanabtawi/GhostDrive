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
// Thread-safe: calls are serialized internally.
class MetadataIndex {
public:
    static Result<MetadataIndex> open(const std::filesystem::path& db_path,
                                      std::uint64_t local_device);

    MetadataIndex(MetadataIndex&&) noexcept;
    MetadataIndex& operator=(MetadataIndex&&) noexcept;
    ~MetadataIndex();

    std::uint64_t local_device() const noexcept;
    std::uint64_t index_id() const noexcept;
    Result<std::uint64_t> max_sequence() const;

    Result<std::optional<FileRecord>> get(std::string_view path) const;
    // Records with sequence > after, in sequence order, tombstones included.
    Result<std::vector<FileRecord>> records_since(std::uint64_t after, std::size_t limit) const;

    // Records a change made on this device. Version, hlc, modified_by and sequence are assigned
    // here; the caller fills content fields.
    Result<FileRecord> commit_local(FileRecord record, HybridClock& clock);
    Result<FileRecord> delete_local(std::string_view path, HybridClock& clock);
    Result<ApplyOutcome> apply_remote(const FileRecord& record, HybridClock& clock);

    // Pinned peers. Only pinned keys may connect; see docs/ARCHITECTURE.md section 7.
    Status add_device(const PublicKey& key, std::string_view name);
    Status remove_device(const PublicKey& key);
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
