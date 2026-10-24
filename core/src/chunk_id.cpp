#include "ghostdrive/core/chunk_id.hpp"

#include <blake3.h>

#include <algorithm>

#include "ghostdrive/core/bytes.hpp"

namespace ghostdrive::core {

static_assert(kDigestSize == BLAKE3_OUT_LEN);
static_assert(kDriveSecretSize == BLAKE3_KEY_LEN);

std::array<std::byte, kChunkIdSize> ChunkId::to_bytes() const noexcept {
    std::array<std::byte, kChunkIdSize> out{};
    out[0] = static_cast<std::byte>(algo);
    std::copy(digest.begin(), digest.end(), out.begin() + 1);
    return out;
}

Result<ChunkId> ChunkId::from_bytes(std::span<const std::byte> bytes) {
    if (bytes.size() != kChunkIdSize) {
        return Error{Errc::invalid_argument, "chunk id has the wrong length"};
    }
    if (static_cast<HashAlgo>(bytes[0]) != HashAlgo::blake3_keyed) {
        return Error{Errc::invalid_argument, "chunk id has an unknown hash algorithm"};
    }
    ChunkId id;
    id.algo = HashAlgo::blake3_keyed;
    std::copy(bytes.begin() + 1, bytes.end(), id.digest.begin());
    return id;
}

std::string ChunkId::to_hex() const {
    return core::to_hex(to_bytes());
}

Result<DriveSecret> generate_drive_secret() {
    DriveSecret secret{};
    if (auto st = random_bytes(secret); !st) return st.error();
    return secret;
}

ChunkHasher::~ChunkHasher() {
    secure_zero(key_);
}

ChunkId ChunkHasher::hash(std::span<const std::byte> data) const noexcept {
    blake3_hasher hasher;
    blake3_hasher_init_keyed(&hasher, reinterpret_cast<const std::uint8_t*>(key_.data()));
    blake3_hasher_update(&hasher, data.data(), data.size());
    ChunkId id;
    blake3_hasher_finalize(&hasher, reinterpret_cast<std::uint8_t*>(id.digest.data()),
                           id.digest.size());
    // The hasher state holds the expanded key.
    secure_zero(std::as_writable_bytes(std::span(&hasher, 1)));
    return id;
}

}  // namespace ghostdrive::core
