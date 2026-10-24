#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ghostdrive/core/result.hpp"

namespace ghostdrive::core {

using Bytes = std::vector<std::byte>;

std::string to_hex(std::span<const std::byte> data);
std::optional<Bytes> from_hex(std::string_view text);

// RFC 4648 base32, upper case, no padding.
std::string base32_encode(std::span<const std::byte> data);
// Inverse of base32_encode; ignores case. Trailing bits that do not fill a byte are dropped.
std::optional<Bytes> base32_decode(std::string_view text);

// Cryptographically secure random bytes (OpenSSL RAND_bytes).
Status random_bytes(std::span<std::byte> out);

// Zeroes memory in a way the compiler cannot elide (OPENSSL_cleanse).
void secure_zero(std::span<std::byte> data) noexcept;

inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span(text.data(), text.size()));
}

}  // namespace ghostdrive::core
