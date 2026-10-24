#include "tls.hpp"

#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <span>
#include <string>

namespace ghostdrive::transport::tls {

using core::Errc;
using core::Error;

namespace {

int select_alpn(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                unsigned int inlen, void*) {
    // `in` is a list of length-prefixed protocol names.
    auto offered = std::span(in, inlen);
    while (!offered.empty()) {
        std::size_t len = offered[0];
        if (len + 1 > offered.size()) break;
        if (len + 1 == kAlpnSize && std::equal(kAlpn + 1, kAlpn + kAlpnSize, offered.begin() + 1)) {
            *out = offered.data() + 1;
            *outlen = static_cast<unsigned char>(len);
            return SSL_TLSEXT_ERR_OK;
        }
        offered = offered.subspan(len + 1);
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

}  // namespace

std::string last_error(const char* what) {
    std::string message(what);
    while (auto code = ERR_get_error()) {
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        message += ": ";
        message += buf;
    }
    return message;
}

core::Result<X509Ptr> make_certificate(const core::DeviceKey& key) {
    X509Ptr cert(X509_new());
    if (!cert) return Error{Errc::crypto, last_error("X509_new")};

    std::uint64_t serial = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&serial), sizeof(serial)) != 1) {
        return Error{Errc::crypto, last_error("RAND_bytes")};
    }
    serial >>= 1;  // keep the ASN.1 INTEGER positive

    constexpr long kDay = 24L * 60 * 60;
    X509_NAME* name = X509_get_subject_name(cert.get());
    if (X509_set_version(cert.get(), X509_VERSION_3) != 1 ||
        ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert.get()), serial) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -kDay) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 20 * 365 * kDay) ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("ghostdrive"), -1, -1,
                                   0) != 1 ||
        X509_set_issuer_name(cert.get(), name) != 1 ||
        X509_set_pubkey(cert.get(), key.native_handle()) != 1) {
        return Error{Errc::crypto, last_error("building certificate")};
    }
    // Ed25519 signs the whole message itself, so no digest is passed.
    if (X509_sign(cert.get(), key.native_handle(), nullptr) <= 0) {
        return Error{Errc::crypto, last_error("X509_sign")};
    }
    return cert;
}

core::Result<SslCtxPtr> make_context(const core::DeviceKey& key, bool server,
                                     SSL_verify_cb verify) {
    static std::once_flag once;
    static int init_result = 0;
    std::call_once(once, [] { init_result = ngtcp2_crypto_ossl_init(); });
    if (init_result != 0) return Error{Errc::crypto, "ngtcp2_crypto_ossl_init failed"};

    SslCtxPtr ctx(SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()));
    if (!ctx) return Error{Errc::crypto, last_error("SSL_CTX_new")};

    auto cert = make_certificate(key);
    if (!cert) return cert.error();

    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) != 1 ||
        SSL_CTX_use_certificate(ctx.get(), cert.value().get()) != 1 ||
        SSL_CTX_use_PrivateKey(ctx.get(), key.native_handle()) != 1 ||
        SSL_CTX_check_private_key(ctx.get()) != 1) {
        return Error{Errc::crypto, last_error("configuring TLS context")};
    }

    // Resumption would skip the certificate exchange that authenticates the peer.
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_OFF);

    if (server) {
        if (SSL_CTX_set_num_tickets(ctx.get(), 0) != 1) {
            return Error{Errc::crypto, last_error("SSL_CTX_set_num_tickets")};
        }
        SSL_CTX_set_alpn_select_cb(ctx.get(), select_alpn, nullptr);
        SSL_CTX_set_verify(
            ctx.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT | SSL_VERIFY_CLIENT_ONCE,
            verify);
    } else {
        // SSL_CTX_set_alpn_protos returns 0 on success.
        if (SSL_CTX_set_alpn_protos(ctx.get(), kAlpn, kAlpnSize) != 0) {
            return Error{Errc::crypto, last_error("SSL_CTX_set_alpn_protos")};
        }
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, verify);
    }
    return ctx;
}

std::optional<core::PublicKey> ed25519_key(const X509* cert) {
    if (cert == nullptr) return std::nullopt;
    EVP_PKEY* pkey = X509_get0_pubkey(cert);
    if (pkey == nullptr || EVP_PKEY_is_a(pkey, "ED25519") != 1) return std::nullopt;
    core::PublicKey key{};
    std::size_t len = key.size();
    if (EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char*>(key.data()), &len) !=
            1 ||
        len != key.size()) {
        return std::nullopt;
    }
    return key;
}

}  // namespace ghostdrive::transport::tls
