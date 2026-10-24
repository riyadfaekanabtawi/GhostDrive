#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunk_id.hpp"

namespace ghostdrive::core {

// Chunk bytes shared between the cache and readers. A reader holding one keeps the bytes alive
// even after the cache evicts them, which is how a pending FUSE reply pins its buffer.
using SharedChunk = std::shared_ptr<const Bytes>;

// Bounded LRU of chunks that have already been hash-verified. Thread-safe.
class ChunkCache {
public:
    explicit ChunkCache(std::size_t capacity_bytes) : capacity_(capacity_bytes) {}

    // Returns the chunk and marks it most recently used, or null.
    SharedChunk get(const ChunkId& id);
    bool contains(const ChunkId& id) const;
    // Inserts or refreshes; evicts least recently used chunks to stay within capacity. A chunk
    // larger than the whole capacity is not cached.
    void put(const ChunkId& id, SharedChunk chunk);
    void erase(const ChunkId& id);

    std::size_t size_bytes() const;
    std::size_t capacity_bytes() const noexcept { return capacity_; }

private:
    struct Entry {
        ChunkId id;
        SharedChunk chunk;
    };
    using List = std::list<Entry>;

    void evict_locked();

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::size_t size_ = 0;
    List lru_;  // front is most recently used
    std::unordered_map<ChunkId, List::iterator> by_id_;
};

}  // namespace ghostdrive::core
