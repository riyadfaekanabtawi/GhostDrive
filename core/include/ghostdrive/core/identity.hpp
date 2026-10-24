#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "ghostdrive/core/result.hpp"

struct evp_pkey_st;

namespace ghostdrive::core {

inline constexpr std::size_t kPublicKeySize = 32;
inline constexpr std::size_t kSignatureSize = 64;
inline constexpr std::size_t kSeedSize = 32;

using PublicKey = std::array<std::byte, kPublicKeySize>;
using Signature = std::array<std::byte, kSignatureSize>;

// A device's long-term Ed25519 identity. The public key is the device ID.
class DeviceKey {
public:
    static Result<DeviceKey> generate();
    static Result<DeviceKey> from_seed(std::span<const std::byte, kSeedSize> seed);

    // Key file: 4-byte magic "GDK1" followed by the 32-byte seed. Must be mode 0600 or stricter.
    static Result<DeviceKey> load(const std::filesystem::path& path);
    // Creates the file with mode 0600; fails with already_exists rather than overwrite a key.
    Status save(const std::filesystem::path& path) const;

    const PublicKey& public_key() const noexcept { return public_key_; }
    Result<Signature> sign(std::span<const std::byte> message) const;

private:
    struct PkeyDeleter {
        void operator()(evp_pkey_st* pkey) const noexcept;
    };
    using Pkey = std::unique_ptr<evp_pkey_st, PkeyDeleter>;

    DeviceKey(Pkey pkey, const PublicKey& public_key);

    Pkey pkey_;
    PublicKey public_key_{};
};

bool verify_signature(const PublicKey& key, std::span<const std::byte> message,
                      const Signature& signature);

// Human-readable device ID: base32 of the public key in dash-separated groups of four.
std::string fingerprint(const PublicKey& key);

// First 64 bits of the public key, big endian. Used as the device ID in version vectors.
std::uint64_t short_id(const PublicKey& key) noexcept;

}  // namespace ghostdrive::core
