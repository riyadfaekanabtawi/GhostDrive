#include "ghostdrive/core/identity.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/file.hpp"
#include "permissions.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::core {
namespace {

template <std::size_t N>
std::array<std::byte, N> hex_array(std::string_view hex) {
    auto bytes = from_hex(hex);
    EXPECT_TRUE(bytes.has_value());
    std::array<std::byte, N> out{};
    if (bytes && bytes->size() == N) std::copy(bytes->begin(), bytes->end(), out.begin());
    return out;
}

// RFC 8032 section 7.1, TEST 1 (empty message).
constexpr std::string_view kRfcSeed =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view kRfcPublic =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
constexpr std::string_view kRfcSignature =
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b4"
    "6"
    "bd25bf5f0595bbe24655141438e7a100b";

TEST(Identity, MatchesRfc8032Vector) {
    const auto seed = hex_array<kSeedSize>(kRfcSeed);
    auto key = DeviceKey::from_seed(seed);
    ASSERT_TRUE(key.ok());
    EXPECT_EQ(to_hex(key.value().public_key()), kRfcPublic);

    auto signature = key.value().sign({});
    ASSERT_TRUE(signature.ok());
    EXPECT_EQ(to_hex(signature.value()), kRfcSignature);
    EXPECT_TRUE(verify_signature(key.value().public_key(), {}, signature.value()));
}

TEST(Identity, SignVerifyAndRejectTampering) {
    auto key = DeviceKey::generate();
    ASSERT_TRUE(key.ok());
    const auto message = as_bytes("pair with device B");
    auto signature = key.value().sign(message);
    ASSERT_TRUE(signature.ok());
    EXPECT_TRUE(verify_signature(key.value().public_key(), message, signature.value()));

    EXPECT_FALSE(verify_signature(key.value().public_key(), as_bytes("pair with device C"),
                                  signature.value()));
    Signature bad = signature.value();
    bad[0] ^= std::byte{0x01};
    EXPECT_FALSE(verify_signature(key.value().public_key(), message, bad));

    auto other = DeviceKey::generate();
    ASSERT_TRUE(other.ok());
    EXPECT_FALSE(verify_signature(other.value().public_key(), message, signature.value()));
}

TEST(Identity, SaveLoadRoundTrip) {
    test::TempDir dir;
    const auto path = dir.path() / "device.key";
    auto key = DeviceKey::generate();
    ASSERT_TRUE(key.ok());
    ASSERT_TRUE(key.value().save(path).ok());

    EXPECT_TRUE(test::has_private_mode(path, 0600));

    auto loaded = DeviceKey::load(path);
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    EXPECT_EQ(loaded.value().public_key(), key.value().public_key());
}

TEST(Identity, SaveRefusesToOverwrite) {
    test::TempDir dir;
    const auto path = dir.path() / "device.key";
    auto first = DeviceKey::generate();
    auto second = DeviceKey::generate();
    ASSERT_TRUE(first.ok() && second.ok());
    ASSERT_TRUE(first.value().save(path).ok());
    auto st = second.value().save(path);
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, Errc::already_exists);
}

TEST(Identity, LoadRejectsPermissiveKeyFile) {
    test::TempDir dir;
    const auto path = dir.path() / "device.key";
    auto key = DeviceKey::generate();
    ASSERT_TRUE(key.ok());
    ASSERT_TRUE(key.value().save(path).ok());
    ASSERT_TRUE(test::let_others_read(path, 0644));
    auto loaded = DeviceKey::load(path);
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, Errc::permission);
}

TEST(Identity, LoadRejectsWrongFormat) {
    test::TempDir dir;
    const auto path = dir.path() / "device.key";
    const char junk[36] = "not a key file, just junk bytes....";
    ASSERT_TRUE(write_private_file(path, std::as_bytes(std::span(junk))));
    auto loaded = DeviceKey::load(path);
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, Errc::corrupt);
}

TEST(Identity, FingerprintAndShortId) {
    const auto pub = hex_array<kPublicKeySize>(kRfcPublic);
    const std::string fp = fingerprint(pub);
    // 32 bytes -> 52 base32 characters in 13 groups of four.
    EXPECT_EQ(fp.size(), 52u + 12u);
    EXPECT_EQ(std::count(fp.begin(), fp.end(), '-'), 12);
    EXPECT_EQ(short_id(pub), 0xd75a980182b10ab7ull);
}

}  // namespace
}  // namespace ghostdrive::core
