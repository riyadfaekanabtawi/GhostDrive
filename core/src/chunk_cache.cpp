#include "ghostdrive/core/chunk_cache.hpp"

namespace ghostdrive::core {

SharedChunk ChunkCache::get(const ChunkId& id) {
    std::lock_guard lock(mutex_);
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->chunk;
}

bool ChunkCache::contains(const ChunkId& id) const {
    std::lock_guard lock(mutex_);
    return by_id_.contains(id);
}

void ChunkCache::put(const ChunkId& id, SharedChunk chunk) {
    if (!chunk) return;
    std::lock_guard lock(mutex_);
    if (auto it = by_id_.find(id); it != by_id_.end()) {
        size_ -= it->second->chunk->size();
        lru_.erase(it->second);
        by_id_.erase(it);
    }
    if (chunk->size() > capacity_) return;
    size_ += chunk->size();
    lru_.push_front(Entry{id, std::move(chunk)});
    by_id_[id] = lru_.begin();
    evict_locked();
}

void ChunkCache::erase(const ChunkId& id) {
    std::lock_guard lock(mutex_);
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return;
    size_ -= it->second->chunk->size();
    lru_.erase(it->second);
    by_id_.erase(it);
}

std::size_t ChunkCache::size_bytes() const {
    std::lock_guard lock(mutex_);
    return size_;
}

void ChunkCache::evict_locked() {
    while (size_ > capacity_ && !lru_.empty()) {
        auto& victim = lru_.back();
        size_ -= victim.chunk->size();
        by_id_.erase(victim.id);
        lru_.pop_back();
    }
}

}  // namespace ghostdrive::core
