#pragma once

#include <cstdint>
#include <optional>

namespace ghostdrive::core {

inline constexpr std::uint32_t kMinBlockSize = 128 * 1024;
inline constexpr std::uint32_t kMaxBlockSize = 16 * 1024 * 1024;
inline constexpr std::uint64_t kMaxBlocksPerFile = 2000;

// Smallest power-of-two block size in [128 KiB, 16 MiB] that keeps the file at or under
// kMaxBlocksPerFile blocks; 16 MiB beyond that. A file keeps its block size when it grows.
std::uint32_t block_size_for(std::uint64_t file_size) noexcept;

std::uint64_t chunk_count(std::uint64_t file_size, std::uint32_t block_size) noexcept;

// Inclusive chunk index range covering bytes [offset, offset + length) clipped to the file.
struct ChunkSpan {
    std::uint64_t first;
    std::uint64_t last;
};

std::optional<ChunkSpan> chunks_for_range(std::uint64_t offset, std::uint64_t length,
                                          std::uint64_t file_size,
                                          std::uint32_t block_size) noexcept;

}  // namespace ghostdrive::core
