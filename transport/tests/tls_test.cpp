#include "tls.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>

namespace ghostdrive::transport::tls {
namespace {

int accept_all(int, X509_STORE_CTX*) {
    return 1;
}

TEST(Tls, CertificateCarriesTheDeviceKey) {
    auto key = core::DeviceKey::generate();
    ASSERT_TRUE(key);
    auto cert = make_certificate(key.value());
    ASSERT_TRUE(cert) << cert.error().message;
    auto extracted = ed25519_key(cert.value().get());
    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(*extracted, key.value().public_key());

    // Self-signed: the certificate verifies under its own key.
    EXPECT_EQ(X509_verify(cert.value().get(), X509_get0_pubkey(cert.value().get())), 1);
}

TEST(Tls, NonEd25519KeysAreNotAccepted) {
    EXPECT_FALSE(ed25519_key(nullptr).has_value());

    EVP_PKEY* ec_key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    ASSERT_NE(ec_key, nullptr);
    X509Ptr cert(X509_new());
    ASSERT_EQ(X509_set_pubkey(cert.get(), ec_key), 1);
    EXPECT_FALSE(ed25519_key(cert.get()).has_value());
    EVP_PKEY_free(ec_key);
}

TEST(Tls, ContextsBuildForBothRoles) {
    auto key = core::DeviceKey::generate();
    ASSERT_TRUE(key);
    auto client = make_context(key.value(), false, accept_all);
    ASSERT_TRUE(client) << client.error().message;
    auto server = make_context(key.value(), true, accept_all);
    ASSERT_TRUE(server) << server.error().message;
    EXPECT_EQ(SSL_CTX_get_min_proto_version(server.value().get()), TLS1_3_VERSION);
}

}  // namespace
}  // namespace ghostdrive::transport::tls
