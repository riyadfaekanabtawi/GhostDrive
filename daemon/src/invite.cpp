#include "ghostdrive/daemon/invite.hpp"

#include <cctype>

#include "ghostdrive/core/bytes.hpp"

namespace ghostdrive::daemon {

using core::Errc;
using core::Error;

namespace {

constexpr std::string_view kPrefix = "gd1-";
constexpr std::uint8_t kFormatVersion = 1;
constexpr std::size_t kMaxAddresses = 16;
constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

std::optional<core::Bytes> base32_decode(std::string_view text) {
    core::Bytes out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        auto pos = kAlphabet.find(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (pos == std::string_view::npos) return std::nullopt;
        buffer = (buffer << 5) | static_cast<std::uint32_t>(pos);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

Error bad_code(const char* why) {
    return Error{Errc::invalid_argument, std::string("invalid invite code: ") + why};
}

}  // namespace

std::string encode_invite(const Invite& invite) {
    core::Bytes raw;
    raw.push_back(static_cast<std::byte>(kFormatVersion));
    raw.insert(raw.end(), invite.inviter.begin(), invite.inviter.end());
    raw.insert(raw.end(), invite.token.begin(), invite.token.end());
    std::size_t count = std::min(invite.addresses.size(), kMaxAddresses);
    raw.push_back(static_cast<std::byte>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const auto& ep = invite.addresses[i];
        if (ep.address().is_v4()) {
            raw.push_back(std::byte{4});
            auto b = ep.address().to_v4().to_bytes();
            for (auto x : b) raw.push_back(static_cast<std::byte>(x));
        } else {
            raw.push_back(std::byte{6});
            auto b = ep.address().to_v6().to_bytes();
            for (auto x : b) raw.push_back(static_cast<std::byte>(x));
        }
        raw.push_back(static_cast<std::byte>(ep.port() >> 8));
        raw.push_back(static_cast<std::byte>(ep.port() & 0xFF));
    }
    auto body = core::base32_encode(raw);
    std::string code(kPrefix);
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (i > 0 && i % 8 == 0) code += '-';
        code += body[i];
    }
    return code;
}

core::Result<Invite> decode_invite(std::string_view code) {
    std::string text;
    for (char c : code) {
        if (c == '-' || std::isspace(static_cast<unsigned char>(c))) continue;
        text += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (text.rfind("GD1", 0) != 0) return bad_code("missing gd1 prefix");
    auto raw = base32_decode(std::string_view(text).substr(3));
    if (!raw) return bad_code("not base32");

    std::span<const std::byte> in(*raw);
    auto take = [&in](std::size_t n) -> std::optional<std::span<const std::byte>> {
        if (in.size() < n) return std::nullopt;
        auto out = in.first(n);
        in = in.subspan(n);
        return out;
    };
    auto version = take(1);
    if (!version || std::to_integer<std::uint8_t>((*version)[0]) != kFormatVersion) {
        return bad_code("unsupported version");
    }
    Invite invite;
    auto key = take(core::kPublicKeySize);
    auto token = take(kTokenSize);
    auto count = take(1);
    if (!key || !token || !count) return bad_code("truncated");
    std::copy(key->begin(), key->end(), invite.inviter.begin());
    std::copy(token->begin(), token->end(), invite.token.begin());
    auto n = std::to_integer<std::size_t>((*count)[0]);
    if (n == 0 || n > kMaxAddresses) return bad_code("no addresses");
    for (std::size_t i = 0; i < n; ++i) {
        auto family = take(1);
        if (!family) return bad_code("truncated address");
        auto f = std::to_integer<std::uint8_t>((*family)[0]);
        asio::ip::address address;
        if (f == 4) {
            auto b = take(4);
            if (!b) return bad_code("truncated address");
            asio::ip::address_v4::bytes_type bytes{};
            for (std::size_t j = 0; j < 4; ++j) bytes[j] = std::to_integer<unsigned char>((*b)[j]);
            address = asio::ip::address_v4(bytes);
        } else if (f == 6) {
            auto b = take(16);
            if (!b) return bad_code("truncated address");
            asio::ip::address_v6::bytes_type bytes{};
            for (std::size_t j = 0; j < 16; ++j) bytes[j] = std::to_integer<unsigned char>((*b)[j]);
            address = asio::ip::address_v6(bytes);
        } else {
            return bad_code("unknown address family");
        }
        auto port = take(2);
        if (!port) return bad_code("truncated address");
        auto p = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>((*port)[0]) << 8 |
                                            std::to_integer<std::uint16_t>((*port)[1]));
        invite.addresses.emplace_back(address, p);
    }
    // Base32 may carry up to 4 padding bits but never a whole extra byte.
    if (!in.empty()) return bad_code("trailing data");
    return invite;
}

}  // namespace ghostdrive::daemon
