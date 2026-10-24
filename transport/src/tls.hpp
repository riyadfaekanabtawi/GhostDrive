#pragma once

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <memory>
#include <optional>

#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"

namespace ghostdrive::transport::tls {

struct X509Deleter {
    void operator()(X509* cert) const noexcept { X509_free(cert); }
};
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

struct SslCtxFree {
    void operator()(SSL_CTX* ctx) const noexcept { SSL_CTX_free(ctx); }
};
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxFree>;

inline constexpr unsigned char kAlpn[] = "\x0cghostdrive/1";
inline constexpr std::size_t kAlpnSize = sizeof(kAlpn) - 1;  // without the terminating NUL

// Self-signed X.509 certificate carrying the device's Ed25519 key. Only the key matters to
// peers; the name and validity fields are placeholders.
core::Result<X509Ptr> make_certificate(const core::DeviceKey& key);

// TLS 1.3 only, ALPN ghostdrive/1, mutual certificates, no session tickets. `verify` sees each
// certificate; it decides on the leaf and must accept the rest of the chain as unverified.
core::Result<SslCtxPtr> make_context(const core::DeviceKey& key, bool server, SSL_verify_cb verify);

// The raw Ed25519 public key of a certificate, or nullopt for any other key type.
std::optional<core::PublicKey> ed25519_key(const X509* cert);

// Drains the OpenSSL error queue into a message.
std::string last_error(const char* what);

}  // namespace ghostdrive::transport::tls
