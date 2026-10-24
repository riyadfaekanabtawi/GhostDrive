#include "ghostdrive/core/bytes.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <limits>

namespace ghostdrive::core {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kBase32Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string to_hex(std::span<const std::byte> data) {
    std::string out;
    out.reserve(data.size() * 2);
    for (std::byte b : data) {
        const auto v = std::to_integer<unsigned>(b);
        out.push_back(kHexDigits[v >> 4]);
        out.push_back(kHexDigits[v & 0xF]);
    }
    return out;
}

std::optional<Bytes> from_hex(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    Bytes out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hex_value(text[i]);
        const int lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::byte>((hi << 4) | lo));
    }
    return out;
}

std::string base32_encode(std::span<const std::byte> data) {
    std::string out;
    out.reserve((data.size() * 8 + 4) / 5);
    unsigned buffer = 0;
    int bits = 0;
    for (std::byte b : data) {
        buffer = (buffer << 8) | std::to_integer<unsigned>(b);
        bits += 8;
        while (bits >= 5) {
            out.push_back(kBase32Alphabet[(buffer >> (bits - 5)) & 0x1F]);
            bits -= 5;
        }
        buffer &= (1u << bits) - 1;
    }
    if (bits > 0) out.push_back(kBase32Alphabet[(buffer << (5 - bits)) & 0x1F]);
    return out;
}

Status random_bytes(std::span<std::byte> out) {
    if (out.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Error{Errc::invalid_argument, "random_bytes: request too large"};
    }
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(out.size())) !=
        1) {
        return Error{Errc::crypto, "RAND_bytes failed"};
    }
    return {};
}

void secure_zero(std::span<std::byte> data) noexcept {
    OPENSSL_cleanse(data.data(), data.size());
}

}  // namespace ghostdrive::core
