#include "ghostdrive/core/chunking.hpp"

namespace ghostdrive::core {

std::uint64_t chunk_count(std::uint64_t file_size, std::uint32_t block_size) noexcept {
    if (block_size == 0) return 0;
    return file_size / block_size + (file_size % block_size != 0 ? 1 : 0);
}

std::uint32_t block_size_for(std::uint64_t file_size) noexcept {
    for (std::uint32_t size = kMinBlockSize; size < kMaxBlockSize; size *= 2) {
        if (chunk_count(file_size, size) <= kMaxBlocksPerFile) return size;
    }
    return kMaxBlockSize;
}

std::optional<ChunkSpan> chunks_for_range(std::uint64_t offset, std::uint64_t length,
                                          std::uint64_t file_size,
                                          std::uint32_t block_size) noexcept {
    if (block_size == 0 || length == 0 || offset >= file_size) return std::nullopt;
    const std::uint64_t end = length > file_size - offset ? file_size : offset + length;
    return ChunkSpan{offset / block_size, (end - 1) / block_size};
}

}  // namespace ghostdrive::core
