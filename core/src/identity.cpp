#include "ghostdrive/core/identity.hpp"

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>

#include <cstring>

#include "ghostdrive/core/bytes.hpp"
#include "posix_file.hpp"

namespace ghostdrive::core {

namespace {

constexpr std::array<char, 4> kKeyFileMagic{'G', 'D', 'K', '1'};
constexpr std::size_t kKeyFileSize = kKeyFileMagic.size() + kSeedSize;

struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};
using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

const unsigned char* uchar_ptr(std::span<const std::byte> data) noexcept {
    // OpenSSL may reject a null pointer even when the length is zero.
    static const unsigned char kEmpty = 0;
    return data.empty() ? &kEmpty : reinterpret_cast<const unsigned char*>(data.data());
}

}  // namespace

void DeviceKey::PkeyDeleter::operator()(evp_pkey_st* pkey) const noexcept {
    EVP_PKEY_free(pkey);
}

DeviceKey::DeviceKey(Pkey pkey, const PublicKey& public_key)
    : pkey_(std::move(pkey)), public_key_(public_key) {}

Result<DeviceKey> DeviceKey::generate() {
    std::array<std::byte, kSeedSize> seed{};
    if (auto st = random_bytes(seed); !st) return st.error();
    auto key = from_seed(seed);
    secure_zero(seed);
    return key;
}

Result<DeviceKey> DeviceKey::from_seed(std::span<const std::byte, kSeedSize> seed) {
    Pkey pkey(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, uchar_ptr(seed), seed.size()));
    if (!pkey) return Error{Errc::crypto, "EVP_PKEY_new_raw_private_key failed"};
    PublicKey public_key{};
    std::size_t len = public_key.size();
    if (EVP_PKEY_get_raw_public_key(pkey.get(), reinterpret_cast<unsigned char*>(public_key.data()),
                                    &len) != 1 ||
        len != public_key.size()) {
        return Error{Errc::crypto, "EVP_PKEY_get_raw_public_key failed"};
    }
    return DeviceKey(std::move(pkey), public_key);
}

Result<DeviceKey> DeviceKey::load(const std::filesystem::path& path) {
    auto fd = detail::open_file(path, O_RDONLY);
    if (!fd) return fd.error();
    struct stat st{};
    if (::fstat(fd.value().get(), &st) != 0) return detail::errno_error("fstat", path);
    if ((st.st_mode & 077) != 0) {
        return Error{Errc::permission,
                     "key file " + path.string() + " is accessible by group or others"};
    }
    if (static_cast<std::uint64_t>(st.st_size) != kKeyFileSize) {
        return Error{Errc::corrupt, "key file " + path.string() + " has the wrong size"};
    }
    std::array<std::byte, kKeyFileSize> buffer{};
    auto n = detail::pread_full(fd.value().get(), buffer, 0, path);
    if (!n || n.value() != buffer.size() ||
        std::memcmp(buffer.data(), kKeyFileMagic.data(), kKeyFileMagic.size()) != 0) {
        secure_zero(buffer);
        return n ? Error{Errc::corrupt, "key file " + path.string() + " is not a GhostDrive key"}
                 : n.error();
    }
    auto key = from_seed(
        std::span<const std::byte, kSeedSize>(buffer.data() + kKeyFileMagic.size(), kSeedSize));
    secure_zero(buffer);
    return key;
}

Status DeviceKey::save(const std::filesystem::path& path) const {
    std::array<std::byte, kKeyFileSize> buffer{};
    std::memcpy(buffer.data(), kKeyFileMagic.data(), kKeyFileMagic.size());
    std::size_t len = kSeedSize;
    if (EVP_PKEY_get_raw_private_key(
            pkey_.get(), reinterpret_cast<unsigned char*>(buffer.data() + kKeyFileMagic.size()),
            &len) != 1 ||
        len != kSeedSize) {
        secure_zero(buffer);
        return Error{Errc::crypto, "EVP_PKEY_get_raw_private_key failed"};
    }
    auto st = detail::write_new_file(path, buffer, 0600);
    secure_zero(buffer);
    return st;
}

Result<Signature> DeviceKey::sign(std::span<const std::byte> message) const {
    MdCtx ctx(EVP_MD_CTX_new());
    if (!ctx) return Error{Errc::crypto, "EVP_MD_CTX_new failed"};
    // Ed25519 is one-shot: no digest type, sign the whole message with EVP_DigestSign.
    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, pkey_.get()) != 1) {
        return Error{Errc::crypto, "EVP_DigestSignInit failed"};
    }
    Signature signature{};
    std::size_t len = signature.size();
    if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(signature.data()), &len,
                       uchar_ptr(message), message.size()) != 1 ||
        len != signature.size()) {
        return Error{Errc::crypto, "EVP_DigestSign failed"};
    }
    return signature;
}

bool verify_signature(const PublicKey& key, std::span<const std::byte> message,
                      const Signature& signature) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, uchar_ptr(key), key.size()),
        &EVP_PKEY_free);
    if (!pkey) return false;
    MdCtx ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
        return false;
    }
    return EVP_DigestVerify(ctx.get(), uchar_ptr(signature), signature.size(), uchar_ptr(message),
                            message.size()) == 1;
}

std::string fingerprint(const PublicKey& key) {
    const std::string encoded = base32_encode(key);
    std::string out;
    out.reserve(encoded.size() + encoded.size() / 4);
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (i != 0 && i % 4 == 0) out.push_back('-');
        out.push_back(encoded[i]);
    }
    return out;
}

std::uint64_t short_id(const PublicKey& key) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value = (value << 8) | std::to_integer<std::uint64_t>(key[i]);
    return value;
}

}  // namespace ghostdrive::core
