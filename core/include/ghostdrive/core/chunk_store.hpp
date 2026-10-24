#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/result.hpp"

namespace ghostdrive::core {

struct ImportedFile {
    std::uint64_t size = 0;
    std::uint32_t block_size = 0;
    std::vector<ChunkId> chunks;
};

// Content-addressed chunk files under <root>/objects/<xx>/<id-hex>. Writes go to <root>/tmp
// and are renamed into place, so readers never see a partial chunk. Safe for concurrent use.
class ChunkStore {
public:
    static Result<ChunkStore> open(std::filesystem::path root, ChunkHasher hasher);

    const ChunkHasher& hasher() const noexcept { return hasher_; }

    Result<ChunkId> put(std::span<const std::byte> data);
    // For chunks received from peers: rejects data whose hash is not `expected`.
    Status put_verified(const ChunkId& expected, std::span<const std::byte> data);

    bool contains(const ChunkId& id) const;
    Result<std::uint64_t> size(const ChunkId& id) const;
    // Reads up to out.size() bytes starting at offset; returns the count (short at end of chunk).
    Result<std::size_t> read(const ChunkId& id, std::uint64_t offset,
                             std::span<std::byte> out) const;
    // Reads the whole chunk and checks it against its ID; corrupt if the bytes changed on disk.
    Result<Bytes> read_verified(const ChunkId& id) const;
    Status remove(const ChunkId& id);

    // Splits a local file into fixed-size chunks (block_size_for) and stores them.
    Result<ImportedFile> import_file(const std::filesystem::path& file);

private:
    ChunkStore(std::filesystem::path root, ChunkHasher hasher);

    std::filesystem::path object_path(const ChunkId& id) const;
    Status write_object(const ChunkId& id, std::span<const std::byte> data);

    std::filesystem::path root_;
    ChunkHasher hasher_;
};

}  // namespace ghostdrive::core
