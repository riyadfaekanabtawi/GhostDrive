#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>

#include "ghostdrive/core/result.hpp"

namespace ghostdrive::core {

enum class HashAlgo : std::uint8_t {
    blake3_keyed = 1,
};

inline constexpr std::size_t kDigestSize = 32;
inline constexpr std::size_t kChunkIdSize = 1 + kDigestSize;
inline constexpr std::size_t kDriveSecretSize = 32;

using DriveSecret = std::array<std::byte, kDriveSecretSize>;

// Content address of a chunk: algorithm tag plus digest. The tag lets the scheme change (for
// example to ciphertext addressing for untrusted devices) without breaking stored IDs.
struct ChunkId {
    HashAlgo algo = HashAlgo::blake3_keyed;
    std::array<std::byte, kDigestSize> digest{};

    friend auto operator<=>(const ChunkId&, const ChunkId&) = default;

    std::array<std::byte, kChunkIdSize> to_bytes() const noexcept;
    static Result<ChunkId> from_bytes(std::span<const std::byte> bytes);
    std::string to_hex() const;
};

Result<DriveSecret> generate_drive_secret();

// BLAKE3 keyed by the drive secret, so outsiders cannot confirm a member holds a known file.
class ChunkHasher {
public:
    explicit ChunkHasher(const DriveSecret& secret) noexcept : key_(secret) {}
    ChunkHasher(const ChunkHasher&) = default;
    ChunkHasher& operator=(const ChunkHasher&) = default;
    ~ChunkHasher();

    ChunkId hash(std::span<const std::byte> data) const noexcept;

private:
    DriveSecret key_;
};

}  // namespace ghostdrive::core

template <>
struct std::hash<ghostdrive::core::ChunkId> {
    std::size_t operator()(const ghostdrive::core::ChunkId& id) const noexcept {
        std::size_t value;
        std::memcpy(&value, id.digest.data(), sizeof(value));
        return value;
    }
};
