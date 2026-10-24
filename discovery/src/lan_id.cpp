#include "ghostdrive/discovery/lan_id.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "ghostdrive/core/bytes.hpp"

namespace ghostdrive::discovery {

namespace {

constexpr std::string_view kLabel = "ghostdrive-lan-v1";

}  // namespace

std::uint64_t lan_epoch(std::chrono::system_clock::time_point now) noexcept {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return seconds < 0 ? 0
                       : static_cast<std::uint64_t>(seconds) /
                             static_cast<std::uint64_t>(kLanEpoch.count());
}

std::string lan_id(const core::DriveSecret& secret, const core::PublicKey& device,
                   std::uint64_t epoch) {
    core::Bytes message;
    auto label = core::as_bytes(kLabel);
    message.insert(message.end(), label.begin(), label.end());
    for (int i = 0; i < 8; ++i) message.push_back(static_cast<std::byte>(epoch >> (8 * i)));
    message.insert(message.end(), device.begin(), device.end());

    std::array<unsigned char, 32> mac{};
    std::size_t mac_len = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, secret.data(), secret.size(),
                  reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                  mac.data(), mac.size(), &mac_len) == nullptr ||
        mac_len != mac.size()) {
        return {};  // never matches anything
    }
    auto truncated = std::as_bytes(std::span(mac.data(), kLanIdSize));
    return core::base32_encode(truncated);
}

std::optional<core::PublicKey> match_lan_id(std::string_view id, const core::DriveSecret& secret,
                                            std::span<const core::PublicKey> members,
                                            std::chrono::system_clock::time_point now) {
    if (id.empty()) return std::nullopt;
    auto epoch = lan_epoch(now);
    for (const auto& member : members) {
        for (std::uint64_t e : {epoch, epoch - 1, epoch + 1}) {
            auto expected = lan_id(secret, member, e);
            if (expected.size() == id.size() &&
                CRYPTO_memcmp(expected.data(), id.data(), id.size()) == 0) {
                return member;
            }
        }
    }
    return std::nullopt;
}

}  // namespace ghostdrive::discovery
