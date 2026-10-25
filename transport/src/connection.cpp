#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <asio/async_result.hpp>
#include <asio/use_awaitable.hpp>
#include <cstdlib>
#include <cstring>

#include "callbacks.hpp"
#include "completion.hpp"
#include "ghostdrive/transport/endpoint.hpp"
#include "tls.hpp"

namespace ghostdrive::transport {

using core::Errc;
using core::Error;
using detail::now;
using detail::post_completion;

namespace {

constexpr std::size_t kCidLen = 18;
// Fits a 1500-byte MTU over IPv4 (1472 bytes of UDP payload) and IPv6 (1452).
constexpr std::size_t kMaxUdpPayload = 1400;
// One relay frame must fit in one 1-RTT packet of the rendezvous connection: a short header
// (1 + 18-byte CID + up to 4-byte packet number), the 16-byte AEAD tag, and the DATAGRAM frame
// header (type and a 2-byte length) take 42 of its 1400 bytes, leaving 1358.
static_assert(kMaxRelayFrame + 1 + kCidLen + 4 + 16 + 3 <= kMaxUdpPayload);
constexpr std::size_t kMaxPacketsPerFlush = 64;
// Received and outgoing DATAGRAM frames held per connection; beyond this they are dropped, and
// QUIC on the relayed connection recovers the loss.
constexpr std::size_t kMaxQueuedDatagrams = 64;
constexpr std::size_t kMaxVectors = 16;

constexpr std::uint64_t kMiB = 1024 * 1024;

Connection* from_user_data(void* user_data) {
    return static_cast<Connection*>(user_data);
}

ngtcp2_duration to_duration(std::chrono::milliseconds ms) {
    return static_cast<ngtcp2_duration>(ms.count()) * NGTCP2_MILLISECONDS;
}

ngtcp2_addr to_ngtcp2(const Udp::endpoint& ep) {
    // ngtcp2 copies the address; the cast only satisfies its non-const signature.
    return ngtcp2_addr{
        .addr = const_cast<ngtcp2_sockaddr*>(reinterpret_cast<const ngtcp2_sockaddr*>(ep.data())),
        .addrlen = static_cast<ngtcp2_socklen>(ep.size()),
    };
}

Udp::endpoint to_endpoint(const ngtcp2_addr& addr, const Udp::endpoint& fallback) {
    Udp::endpoint ep;
    if (addr.addr == nullptr || addr.addrlen == 0 || addr.addrlen > ep.capacity()) return fallback;
    std::memcpy(ep.data(), addr.addr, addr.addrlen);
    ep.resize(addr.addrlen);
    return ep;
}

Status random_cid(ngtcp2_cid& cid, std::size_t len) {
    cid.datalen = len;
    if (RAND_bytes(cid.data, static_cast<int>(len)) != 1) {
        return Error{Errc::crypto, "RAND_bytes failed"};
    }
    return {};
}

// `relayed`: the remote is a relay virtual address. `datagrams`: the connection to a rendezvous
// server, which carries relay frames.
void fill_settings(ngtcp2_settings& settings, const EndpointOptions& options, bool relayed,
                   bool datagrams) {
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now();
    settings.handshake_timeout = to_duration(options.handshake_timeout);
    settings.max_tx_udp_payload_size = relayed ? kMaxRelayedPacket : kMaxUdpPayload;
    settings.max_window = 64 * kMiB;
    settings.max_stream_window = 16 * kMiB;
    if (relayed || datagrams) {
        // Otherwise ngtcp2 sends 1200-byte packets until Path MTU Discovery finds more, and a
        // relay frame holding a 1200-byte Initial does not fit in 1200 bytes. The sizes are
        // fixed instead: the rendezvous connection uses the 1400 bytes direct connections reach
        // at most, and relayed packets always fit inside its frames.
        settings.no_tx_udp_payload_size_shaping = 1;
        settings.no_pmtud = 1;
    }
}

void fill_params(ngtcp2_transport_params& params, const EndpointOptions& options, bool relayed,
                 bool datagrams) {
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = kMiB;
    params.initial_max_stream_data_bidi_remote = kMiB;
    params.initial_max_stream_data_uni = 0;
    params.initial_max_data = 4 * kMiB;
    params.initial_max_streams_bidi = 128;
    params.initial_max_streams_uni = 0;
    params.max_idle_timeout = to_duration(options.idle_timeout);
    params.active_connection_id_limit = 4;
    // The demux tells QUIC from our own datagrams by the fixed bit, so it must not be greased.
    params.grease_quic_bit = 0;
    // Paths change only by NAT rebinding (handled by ngtcp2) or by a new connection; never let
    // a peer move a connection, which would also slip a direct one onto the relay unnoticed.
    params.disable_active_migration = 1;
    if (relayed) params.max_udp_payload_size = kMaxRelayedPacket;
    // The largest frame a 1400-byte packet could hold; send_datagram enforces the smaller limit.
    if (datagrams) params.max_datagram_frame_size = kMaxUdpPayload;
}

}  // namespace

// ---- Callbacks ------------------------------------------------------------------------------

const ngtcp2_callbacks Callbacks::client = {
    .client_initial = ngtcp2_crypto_client_initial_cb,
    .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
    .handshake_completed = Callbacks::handshake_completed,
    .encrypt = ngtcp2_crypto_encrypt_cb,
    .decrypt = ngtcp2_crypto_decrypt_cb,
    .hp_mask = ngtcp2_crypto_hp_mask_cb,
    .recv_stream_data = Callbacks::recv_stream_data,
    .acked_stream_data_offset = Callbacks::acked_stream_data_offset,
    .stream_open = Callbacks::stream_open,
    .recv_retry = ngtcp2_crypto_recv_retry_cb,
    .extend_max_local_streams_bidi = Callbacks::extend_max_local_streams_bidi,
    .rand = Callbacks::rand,
    .remove_connection_id = Callbacks::remove_connection_id,
    .update_key = ngtcp2_crypto_update_key_cb,
    .stream_reset = Callbacks::stream_reset,
    .handshake_confirmed = Callbacks::handshake_confirmed,
    .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .recv_datagram = Callbacks::recv_datagram,
    .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
    .get_new_connection_id2 = Callbacks::get_new_connection_id,
    .get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb,
    .stream_close2 = Callbacks::stream_close,
};

const ngtcp2_callbacks Callbacks::server = {
    .recv_client_initial = ngtcp2_crypto_recv_client_initial_cb,
    .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
    .handshake_completed = Callbacks::handshake_completed,
    .encrypt = ngtcp2_crypto_encrypt_cb,
    .decrypt = ngtcp2_crypto_decrypt_cb,
    .hp_mask = ngtcp2_crypto_hp_mask_cb,
    .recv_stream_data = Callbacks::recv_stream_data,
    .acked_stream_data_offset = Callbacks::acked_stream_data_offset,
    .stream_open = Callbacks::stream_open,
    .extend_max_local_streams_bidi = Callbacks::extend_max_local_streams_bidi,
    .rand = Callbacks::rand,
    .remove_connection_id = Callbacks::remove_connection_id,
    .update_key = ngtcp2_crypto_update_key_cb,
    .stream_reset = Callbacks::stream_reset,
    .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .recv_datagram = Callbacks::recv_datagram,
    .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
    .get_new_connection_id2 = Callbacks::get_new_connection_id,
    .get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb,
    .stream_close2 = Callbacks::stream_close,
};

ngtcp2_conn* Callbacks::get_conn(ngtcp2_crypto_conn_ref* ref) {
    return static_cast<Connection*>(ref->user_data)->conn_;
}

void Callbacks::rand(std::uint8_t* dest, std::size_t len, const ngtcp2_rand_ctx*) {
    // ngtcp2 gives this callback no way to report failure, and continuing without randomness
    // would be unsafe.
    if (RAND_bytes(dest, static_cast<int>(len)) != 1) std::abort();
}

int Callbacks::get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid,
                                     ngtcp2_stateless_reset_token* token, std::size_t cidlen,
                                     void* user_data) {
    auto* self = from_user_data(user_data);
    auto endpoint = self->endpoint_.lock();
    if (!endpoint || !random_cid(*cid, cidlen)) return NGTCP2_ERR_CALLBACK_FAILURE;
    if (ngtcp2_crypto_generate_stateless_reset_token(token->data, endpoint->static_secret_.data(),
                                                     endpoint->static_secret_.size(), cid) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    endpoint->register_cid({cid->data, cid->datalen}, self->shared_from_this());
    return 0;
}

int Callbacks::remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) {
    auto* self = from_user_data(user_data);
    if (auto endpoint = self->endpoint_.lock()) endpoint->unregister_cid({cid->data, cid->datalen});
    return 0;
}

int Callbacks::handshake_completed(ngtcp2_conn*, void* user_data) {
    auto* self = from_user_data(user_data);
    if (auto st = self->check_peer_certificate(); !st) {
        self->callback_error_ = st.error();
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    // A client waits for HANDSHAKE_DONE: the server may still reject our certificate after
    // our side of the TLS handshake has finished.
    if (!self->is_client_) self->handshake_done_ = true;
    return 0;
}

int Callbacks::handshake_confirmed(ngtcp2_conn*, void* user_data) {
    from_user_data(user_data)->handshake_done_ = true;
    return 0;
}

int Callbacks::recv_stream_data(ngtcp2_conn*, std::uint32_t flags, std::int64_t, std::uint64_t,
                                const std::uint8_t* data, std::size_t datalen, void*,
                                void* stream_user_data) {
    // A stream reset locally has no user data; ngtcp2 discards what still arrives for it.
    if (auto* stream = static_cast<Stream*>(stream_user_data)) {
        stream->on_data(std::as_bytes(std::span(data, datalen)),
                        (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0);
    }
    return 0;
}

int Callbacks::stream_open(ngtcp2_conn* conn, std::int64_t stream_id, void* user_data) {
    // Only called for peer-initiated streams; unidirectional ones are disabled by the
    // transport parameters.
    if (!ngtcp2_is_bidi_stream(stream_id)) return 0;
    auto* self = from_user_data(user_data);
    auto stream = self->add_stream(stream_id);
    if (ngtcp2_conn_set_stream_user_data(conn, stream_id, stream.get()) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    self->incoming_.push_back(std::move(stream));
    return 0;
}

int Callbacks::acked_stream_data_offset(ngtcp2_conn*, std::int64_t, std::uint64_t offset,
                                        std::uint64_t datalen, void*, void* stream_user_data) {
    if (auto* stream = static_cast<Stream*>(stream_user_data)) stream->on_acked(offset, datalen);
    return 0;
}

int Callbacks::stream_close(ngtcp2_conn* conn, std::uint32_t flags, std::int64_t stream_id,
                            std::uint64_t rx_app_error_code, std::uint64_t, void* user_data,
                            void* stream_user_data) {
    auto* self = from_user_data(user_data);
    if (auto* stream = static_cast<Stream*>(stream_user_data)) {
        bool reset =
            (flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) != 0 && rx_app_error_code != 0;
        stream->on_closed(Error{Errc::closed, reset ? "stream reset by peer" : "stream closed"});
    }
    self->streams_.erase(stream_id);
    if (ngtcp2_is_bidi_stream(stream_id) && !ngtcp2_conn_is_local_stream(conn, stream_id)) {
        ngtcp2_conn_extend_max_streams_bidi(conn, 1);
    }
    return 0;
}

int Callbacks::stream_reset(ngtcp2_conn*, std::int64_t, std::uint64_t, std::uint64_t, void*,
                            void* stream_user_data) {
    if (auto* stream = static_cast<Stream*>(stream_user_data)) {
        stream->on_closed(Error{Errc::closed, "stream reset by peer"});
    }
    return 0;
}

int Callbacks::extend_max_local_streams_bidi(ngtcp2_conn*, std::uint64_t, void* user_data) {
    from_user_data(user_data)->streams_credit_ = true;
    return 0;
}

int Callbacks::recv_datagram(ngtcp2_conn*, std::uint32_t flags, const std::uint8_t* data,
                             std::size_t datalen, void* user_data) {
    auto* self = from_user_data(user_data);
    // Before the handshake completes the peer is not authenticated (0-RTT is never enabled).
    if ((flags & NGTCP2_DATAGRAM_FLAG_0RTT) != 0 || !self->datagrams_) return 0;
    if (self->rx_datagrams_.size() >= kMaxQueuedDatagrams) return 0;
    // COPY: ngtcp2's decrypted packet is valid only during this callback, and the frame is
    // handled after ngtcp2 returns.
    auto* bytes = reinterpret_cast<const std::byte*>(data);
    self->rx_datagrams_.emplace_back(bytes, bytes + datalen);
    return 0;
}

int Callbacks::verify_certificate(int, X509_STORE_CTX* store) {
    // Chain elements above the leaf carry no trust here; only the leaf key is checked. This runs
    // before CertificateVerify, so the peer has not yet proven it holds the key: only the pure
    // comparison against a dialed key happens here. accept_peer, which may have side effects,
    // runs in check_peer_certificate after the handshake.
    if (X509_STORE_CTX_get_error_depth(store) != 0) return 1;
    auto* ssl =
        static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    if (ssl == nullptr) return 0;
    auto* ref = static_cast<ngtcp2_crypto_conn_ref*>(SSL_get_app_data(ssl));
    if (ref == nullptr) return 0;
    auto key = tls::ed25519_key(X509_STORE_CTX_get_current_cert(store));
    if (!key) return 0;
    const auto& expected = static_cast<Connection*>(ref->user_data)->expected_peer_;
    return !expected || *key == *expected ? 1 : 0;
}

// ---- Connection -----------------------------------------------------------------------------

Connection::Connection(Private, const std::shared_ptr<Endpoint>& endpoint,
                       const Udp::endpoint& remote, bool is_client)
    : endpoint_(endpoint),
      executor_(endpoint->executor_),
      local_(endpoint->local_endpoint()),
      remote_(remote),
      is_client_(is_client),
      timer_(endpoint->executor_) {}

Connection::~Connection() {
    if (conn_ != nullptr) ngtcp2_conn_del(conn_);
    if (ossl_ctx_ != nullptr) {
        if (SSL* ssl = ngtcp2_crypto_ossl_ctx_get_ssl(ossl_ctx_)) {
            SSL_set_app_data(ssl, nullptr);
            SSL_free(ssl);
        }
        ngtcp2_crypto_ossl_ctx_del(ossl_ctx_);
    }
}

Result<std::shared_ptr<Connection>> Connection::create_client(
    const std::shared_ptr<Endpoint>& endpoint, const Udp::endpoint& remote,
    const core::PublicKey& expected_peer, ConnectOptions options) {
    auto self = std::make_shared<Connection>(Private{}, endpoint, remote, true);
    self->expected_peer_ = expected_peer;
    self->datagrams_ = options.datagrams;
    bool relayed = endpoint->is_relay_address(remote);

    ngtcp2_cid dcid{};
    ngtcp2_cid scid{};
    if (auto st = random_cid(dcid, kCidLen); !st) return st.error();
    if (auto st = random_cid(scid, kCidLen); !st) return st.error();

    ngtcp2_settings settings;
    fill_settings(settings, endpoint->options_, relayed, self->datagrams_);
    ngtcp2_transport_params params;
    fill_params(params, endpoint->options_, relayed, self->datagrams_);

    ngtcp2_path path{
        .local = to_ngtcp2(self->local_), .remote = to_ngtcp2(remote), .user_data = nullptr};
    if (int rv =
            ngtcp2_conn_client_new(&self->conn_, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1,
                                   &Callbacks::client, &settings, &params, nullptr, self.get());
        rv != 0) {
        self->conn_ = nullptr;
        return Error{Errc::protocol, std::string("ngtcp2_conn_client_new: ") + ngtcp2_strerror(rv)};
    }
    if (auto st = self->init_tls(endpoint->client_ctx_.get()); !st) return st.error();
    if (endpoint->options_.keep_alive.count() > 0) {
        ngtcp2_conn_set_keep_alive_timeout(self->conn_, to_duration(endpoint->options_.keep_alive));
    }
    endpoint->register_cid({scid.data, scid.datalen}, self);
    return self;
}

Result<std::shared_ptr<Connection>> Connection::create_server(
    const std::shared_ptr<Endpoint>& endpoint, const Udp::endpoint& remote,
    std::span<const std::uint8_t> initial_packet,
    const std::optional<std::vector<std::uint8_t>>& original_dcid) {
    ngtcp2_pkt_hd hd;
    if (ngtcp2_accept(&hd, initial_packet.data(), initial_packet.size()) != 0) {
        return Error{Errc::protocol, "not an acceptable Initial packet"};
    }
    auto self = std::make_shared<Connection>(Private{}, endpoint, remote, false);
    self->datagrams_ = endpoint->options_.accept_datagrams;
    bool relayed = endpoint->is_relay_address(remote);

    ngtcp2_cid scid{};
    if (auto st = random_cid(scid, kCidLen); !st) return st.error();

    ngtcp2_settings settings;
    fill_settings(settings, endpoint->options_, relayed, self->datagrams_);
    ngtcp2_transport_params params;
    fill_params(params, endpoint->options_, relayed, self->datagrams_);
    if (original_dcid) {
        // After a Retry the client addresses the CID we put in the Retry (now its DCID); the
        // transport parameters must name both, so the client can check nobody tampered.
        ngtcp2_cid_init(&params.original_dcid, original_dcid->data(), original_dcid->size());
        params.retry_scid = hd.dcid;
        params.retry_scid_present = 1;
        settings.token = hd.token;
        settings.tokenlen = hd.tokenlen;
        settings.token_type = NGTCP2_TOKEN_TYPE_RETRY;
    } else {
        params.original_dcid = hd.dcid;
    }
    params.original_dcid_present = 1;
    params.stateless_reset_token_present = 1;
    if (ngtcp2_crypto_generate_stateless_reset_token(params.stateless_reset_token,
                                                     endpoint->static_secret_.data(),
                                                     endpoint->static_secret_.size(), &scid) != 0) {
        return Error{Errc::crypto, "could not generate a stateless reset token"};
    }

    ngtcp2_path path{
        .local = to_ngtcp2(self->local_), .remote = to_ngtcp2(remote), .user_data = nullptr};
    if (int rv =
            ngtcp2_conn_server_new(&self->conn_, &hd.scid, &scid, &path, hd.version,
                                   &Callbacks::server, &settings, &params, nullptr, self.get());
        rv != 0) {
        self->conn_ = nullptr;
        return Error{Errc::protocol, std::string("ngtcp2_conn_server_new: ") + ngtcp2_strerror(rv)};
    }
    if (auto st = self->init_tls(endpoint->server_ctx_.get()); !st) return st.error();
    if (endpoint->options_.keep_alive.count() > 0) {
        ngtcp2_conn_set_keep_alive_timeout(self->conn_, to_duration(endpoint->options_.keep_alive));
    }
    endpoint->register_cid({scid.data, scid.datalen}, self);
    // The client keeps addressing the original DCID until it sees our first packet.
    endpoint->register_cid({hd.dcid.data, hd.dcid.datalen}, self);
    self->pending_handshake_ = true;
    ++endpoint->pending_handshakes_;
    return self;
}

Status Connection::init_tls(ssl_ctx_st* ssl_ctx) {
    conn_ref_ = std::make_unique<ngtcp2_crypto_conn_ref>();
    conn_ref_->get_conn = Callbacks::get_conn;
    conn_ref_->user_data = this;

    if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx_, nullptr) != 0) {
        ossl_ctx_ = nullptr;
        return Error{Errc::crypto, "ngtcp2_crypto_ossl_ctx_new failed"};
    }
    SSL* ssl = SSL_new(ssl_ctx);
    if (ssl == nullptr) return Error{Errc::crypto, tls::last_error("SSL_new")};
    ngtcp2_crypto_ossl_ctx_set_ssl(ossl_ctx_, ssl);

    int rv = is_client_ ? ngtcp2_crypto_ossl_configure_client_session(ssl)
                        : ngtcp2_crypto_ossl_configure_server_session(ssl);
    if (rv != 0) return Error{Errc::crypto, "configuring the QUIC TLS session failed"};

    SSL_set_app_data(ssl, conn_ref_.get());
    if (is_client_) {
        SSL_set_connect_state(ssl);
    } else {
        SSL_set_accept_state(ssl);
    }
    ngtcp2_conn_set_tls_native_handle(conn_, ossl_ctx_);
    return {};
}

Udp::endpoint Connection::current_remote_endpoint() const {
    if (conn_ == nullptr) return remote_;
    return to_endpoint(ngtcp2_conn_get_path2(conn_)->remote, remote_);
}

bool Connection::authorize(const core::PublicKey& key) const {
    if (expected_peer_) return key == *expected_peer_;
    auto endpoint = endpoint_.lock();
    if (!endpoint || !endpoint->options_.accept_peer) return false;
    // A relayed connection must be with the key its virtual address stands for: otherwise the
    // server could present any key, even a non-member's, at a member's address. Checked before
    // accept_peer, which may have side effects (pairing).
    if (auto it = endpoint->relay_peer_.find(remote_);
        it != endpoint->relay_peer_.end() && it->second != key) {
        return false;
    }
    // Called from inside ngtcp2's handshake callback: an exception must not unwind through C.
    try {
        return endpoint->options_.accept_peer(key);
    } catch (...) {
        return false;
    }
}

Status Connection::check_peer_certificate() {
    SSL* ssl = ngtcp2_crypto_ossl_ctx_get_ssl(ossl_ctx_);
    if (ssl == nullptr) return Error{Errc::crypto, "no TLS session"};

    const unsigned char* alpn = nullptr;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn_len + 1 != tls::kAlpnSize || std::memcmp(alpn, tls::kAlpn + 1, alpn_len) != 0) {
        return Error{Errc::protocol, "peer did not negotiate ALPN ghostdrive/1"};
    }

    // Authoritative pin check; the verify callback already rejected bad keys during the
    // handshake, this guards against any path where it was not consulted.
    auto key = tls::ed25519_key(SSL_get0_peer_certificate(ssl));
    if (!key) return Error{Errc::permission, "peer did not present an Ed25519 certificate"};
    if (!authorize(*key)) return Error{Errc::permission, "peer key is not pinned"};
    peer_key_ = *key;
    return {};
}

asio::awaitable<Result<std::shared_ptr<Stream>>> Connection::open_stream() {
    co_return co_await asio::async_initiate<const asio::use_awaitable_t<>,
                                            void(Result<std::shared_ptr<Stream>>)>(
        [this](auto handler) { start_open_stream(StreamHandler(std::move(handler))); },
        asio::use_awaitable);
}

asio::awaitable<Result<std::shared_ptr<Stream>>> Connection::accept_stream() {
    co_return co_await asio::async_initiate<const asio::use_awaitable_t<>,
                                            void(Result<std::shared_ptr<Stream>>)>(
        [this](auto handler) { start_accept_stream(StreamHandler(std::move(handler))); },
        asio::use_awaitable);
}

void Connection::start_open_stream(StreamHandler handler) {
    if (closed_) {
        post_completion(executor_, handler,
                        Result<std::shared_ptr<Stream>>(Error{Errc::closed, "connection closed"}));
        return;
    }
    open_waiters_.push_back(std::move(handler));
    after_events();
}

void Connection::start_accept_stream(StreamHandler handler) {
    if (accept_handler_) {
        post_completion(executor_, handler,
                        Result<std::shared_ptr<Stream>>(
                            Error{Errc::invalid_argument, "accept_stream already pending"}));
        return;
    }
    if (closed_ && incoming_.empty()) {
        post_completion(executor_, handler,
                        Result<std::shared_ptr<Stream>>(Error{Errc::closed, "connection closed"}));
        return;
    }
    accept_handler_ = std::move(handler);
    after_events();
}

std::shared_ptr<Stream> Connection::add_stream(std::int64_t id) {
    auto stream = std::make_shared<Stream>(Stream::Private{}, weak_from_this(), executor_, id);
    streams_[id] = stream;
    return stream;
}

void Connection::enqueue_send(std::int64_t id) {
    if (std::find(send_queue_.begin(), send_queue_.end(), id) == send_queue_.end()) {
        send_queue_.push_back(id);
    }
}

void Connection::after_events() {
    auto self = shared_from_this();

    if (handshake_done_ && !announced_ && !closed_) {
        announced_ = true;
        if (is_client_) {
            post_completion(executor_, connect_handler_, Result<std::shared_ptr<Connection>>(self));
        } else if (auto endpoint = endpoint_.lock()) {
            release_pending_handshake();
            endpoint->on_established(self);
        }
    }

    if (accept_handler_ && !incoming_.empty()) {
        auto stream = std::move(incoming_.front());
        incoming_.pop_front();
        post_completion(executor_, accept_handler_, Result<std::shared_ptr<Stream>>(stream));
    }

    streams_credit_ = false;
    while (!open_waiters_.empty() && !closed_ && announced_) {
        std::int64_t id = -1;
        int rv = ngtcp2_conn_open_bidi_stream(conn_, &id, nullptr);
        if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) break;  // resumes on extend_max_local_streams
        auto handler = std::move(open_waiters_.front());
        open_waiters_.pop_front();
        if (rv != 0) {
            post_completion(executor_, handler,
                            Result<std::shared_ptr<Stream>>(Error{
                                Errc::protocol, std::string("ngtcp2_conn_open_bidi_stream: ") +
                                                    ngtcp2_strerror(rv)}));
            continue;
        }
        auto stream = add_stream(id);
        ngtcp2_conn_set_stream_user_data(conn_, id, stream.get());
        post_completion(executor_, handler, Result<std::shared_ptr<Stream>>(stream));
    }

    deliver_datagrams();
}

void Connection::deliver_datagrams() {
    if (!announced_ || rx_datagrams_.empty()) return;
    auto self = shared_from_this();
    auto received = std::move(rx_datagrams_);
    rx_datagrams_.clear();
    auto handler = datagram_handler_;
    for (auto& datagram : received) {
        if (closed_ || !handler) break;
        handler(std::move(datagram));
    }
}

bool Connection::send_datagram(core::Bytes datagram) {
    if (!datagrams_ || closed_ || datagram.empty() || datagram.size() > kMaxDatagram ||
        tx_datagrams_.size() >= kMaxQueuedDatagrams) {
        return false;
    }
    tx_datagrams_.push_back(std::move(datagram));
    schedule_flush();
    return true;
}

void Connection::on_packet(std::span<const std::uint8_t> packet, const Udp::endpoint& from) {
    auto self = shared_from_this();
    if (closed_) return;

    ngtcp2_path path{.local = to_ngtcp2(local_), .remote = to_ngtcp2(from), .user_data = nullptr};
    ngtcp2_pkt_info pi{};
    int rv = ngtcp2_conn_read_pkt(conn_, &path, &pi, packet.data(), packet.size(), now());
    if (rv != 0) {
        switch (rv) {
            case NGTCP2_ERR_DRAINING:
                fail(Error{Errc::closed, "connection closed by peer"}, false);
                return;
            case NGTCP2_ERR_DROP_CONN:
                fail(Error{Errc::closed, "connection dropped"}, false);
                return;
            case NGTCP2_ERR_CRYPTO:
                close_tls_alert_ = ngtcp2_conn_get_tls_alert2(conn_);
                fail(callback_error_
                         ? *callback_error_
                         : Error{Errc::crypto, "TLS handshake failed (alert " +
                                                   std::to_string(close_tls_alert_) + ")"},
                     true);
                return;
            default:
                close_liberr_ = rv;
                fail(callback_error_ ? *callback_error_
                                     : Error{Errc::protocol, std::string("ngtcp2_conn_read_pkt: ") +
                                                                 ngtcp2_strerror(rv)},
                     true);
                return;
        }
    }
    after_events();
    flush();
}

void Connection::on_timer() {
    auto self = shared_from_this();
    if (closed_) return;
    int rv = ngtcp2_conn_handle_expiry(conn_, now());
    if (rv != 0) {
        switch (rv) {
            case NGTCP2_ERR_IDLE_CLOSE:
                fail(Error{Errc::timeout, "connection idle timeout"}, false);
                return;
            case NGTCP2_ERR_HANDSHAKE_TIMEOUT:
                fail(Error{Errc::timeout, "handshake timed out"}, false);
                return;
            default:
                close_liberr_ = rv;
                fail(Error{Errc::protocol,
                           std::string("ngtcp2_conn_handle_expiry: ") + ngtcp2_strerror(rv)},
                     true);
                return;
        }
    }
    after_events();
    flush();
}

void Connection::schedule_flush() {
    if (flush_scheduled_ || closed_) return;
    flush_scheduled_ = true;
    asio::post(executor_, [weak = weak_from_this()] {
        if (auto self = weak.lock()) self->flush();
    });
}

void Connection::flush() {
    auto self = shared_from_this();
    flush_scheduled_ = false;
    if (closed_ || conn_ == nullptr) return;
    auto endpoint = endpoint_.lock();
    if (!endpoint) return;

    std::array<std::uint8_t, kMaxUdpPayload> buffer;
    std::array<std::span<const std::byte>, kMaxVectors> spans;
    std::array<ngtcp2_vec, kMaxVectors> vectors;

    std::size_t max_packets = std::clamp<std::size_t>(
        ngtcp2_conn_get_send_quantum2(conn_) /
            std::max<std::size_t>(1, ngtcp2_conn_get_path_max_tx_udp_payload_size2(conn_)),
        1, kMaxPacketsPerFlush);

    ngtcp2_tstamp ts = now();
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi{};

    // Streams with something to send, in queue order. A stream that is flow-control blocked is
    // skipped for the rest of this flush and retried on the next one.
    std::vector<std::int64_t> candidates(send_queue_.begin(), send_queue_.end());
    std::size_t next = 0;
    std::size_t sent = 0;

    // DATAGRAM frames, one per packet. They are never retransmitted; those congestion control
    // or pacing holds back wait in the bounded queue. While streams have data, frames get at
    // most half of the round first and only what the streams leave after, so a flood of relay
    // frames cannot starve the control stream. Returns false if the connection failed.
    auto write_datagrams = [&](std::size_t limit) {
        bool retried = false;
        for (std::size_t n = 0;
             announced_ && !tx_datagrams_.empty() && n < limit && sent < max_packets;) {
            auto& datagram = tx_datagrams_.front();
            ngtcp2_vec vector{
                .base = reinterpret_cast<std::uint8_t*>(datagram.data()),
                .len = datagram.size(),
            };
            int accepted = 0;
            auto written = ngtcp2_conn_writev_datagram(
                conn_, &ps.path, &pi, buffer.data(), buffer.size(), &accepted,
                NGTCP2_WRITE_DATAGRAM_FLAG_NONE, 0, &vector, 1, ts);
            if (written == NGTCP2_ERR_INVALID_STATE || written == NGTCP2_ERR_INVALID_ARGUMENT) {
                // The peer takes no DATAGRAM frames, or none this large.
                tx_datagrams_.pop_front();
                continue;
            }
            if (written < 0) {
                close_liberr_ = static_cast<int>(written);
                fail(Error{Errc::protocol, std::string("ngtcp2_conn_writev_datagram: ") +
                                               ngtcp2_strerror(static_cast<int>(written))},
                     true);
                return false;
            }
            // Written but not accepted: other frames (ACKs) took the room, so the frame goes in
            // the next packet. One that does not fit a second packet either is dropped, not left
            // at the head of the queue.
            if (accepted != 0 || (written > 0 && retried)) {
                tx_datagrams_.pop_front();
                retried = false;
            } else {
                retried = written > 0;
            }
            if (written == 0) break;  // congestion control or pacing says wait
            endpoint->send_packet(
                std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(written))),
                to_endpoint(ps.path.remote, remote_));
            ++sent;
            ++n;
        }
        return true;
    };
    auto share = candidates.empty() ? max_packets : std::max<std::size_t>(1, max_packets / 2);
    if (!write_datagrams(share)) return;

    for (;;) {
        Stream* stream = nullptr;
        while (next < candidates.size()) {
            auto it = streams_.find(candidates[next]);
            if (it != streams_.end() && (it->second->has_unsent() || it->second->wants_fin())) {
                stream = it->second.get();
                break;
            }
            ++next;
        }

        std::int64_t stream_id = -1;
        std::size_t vector_count = 0;
        std::uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if (stream != nullptr) {
            stream_id = stream->id();
            vector_count = stream->unsent_vectors(spans);
            for (std::size_t i = 0; i < vector_count; ++i) {
                // ngtcp2 reads (never writes) through this pointer.
                vectors[i].base = const_cast<std::uint8_t*>(
                    reinterpret_cast<const std::uint8_t*>(spans[i].data()));
                vectors[i].len = spans[i].size();
            }
            if (stream->wants_fin()) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        }

        ngtcp2_ssize consumed = -1;
        auto written =
            ngtcp2_conn_writev_stream(conn_, &ps.path, &pi, buffer.data(), buffer.size(), &consumed,
                                      flags, stream_id, vectors.data(), vector_count, ts);
        bool fin_flagged = (flags & NGTCP2_WRITE_STREAM_FLAG_FIN) != 0;

        if (written < 0) {
            switch (written) {
                case NGTCP2_ERR_WRITE_MORE:
                    stream->on_sent(static_cast<std::size_t>(consumed), fin_flagged);
                    if (!stream->has_unsent() && !stream->wants_fin()) ++next;
                    continue;
                case NGTCP2_ERR_STREAM_DATA_BLOCKED:
                    ++next;
                    continue;
                case NGTCP2_ERR_STREAM_SHUT_WR:
                case NGTCP2_ERR_STREAM_NOT_FOUND:
                    stream->on_closed(Error{Errc::closed, "stream is closed for writing"});
                    ++next;
                    continue;
                default:
                    close_liberr_ = static_cast<int>(written);
                    fail(Error{Errc::protocol, std::string("ngtcp2_conn_writev_stream: ") +
                                                   ngtcp2_strerror(static_cast<int>(written))},
                         true);
                    return;
            }
        }

        if (stream != nullptr && consumed >= 0) {
            stream->on_sent(static_cast<std::size_t>(consumed), fin_flagged);
            if (!stream->has_unsent() && !stream->wants_fin()) ++next;
        }
        if (written == 0) break;  // nothing left, or congestion and pacing say wait

        endpoint->send_packet(
            std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(written))),
            to_endpoint(ps.path.remote, remote_));
        if (++sent >= max_packets) break;
    }
    if (!write_datagrams(max_packets)) return;
    ngtcp2_conn_update_pkt_tx_time(conn_, ts);

    std::erase_if(send_queue_, [this](std::int64_t id) {
        auto it = streams_.find(id);
        return it == streams_.end() || (!it->second->has_unsent() && !it->second->wants_fin());
    });
    arm_timer();
}

void Connection::arm_timer() {
    if (closed_ || conn_ == nullptr) return;
    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry2(conn_);
    if (expiry == UINT64_MAX) {
        timer_.cancel();
        return;
    }
    timer_.expires_at(std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(expiry))));
    timer_.async_wait([weak = weak_from_this()](const std::error_code& ec) {
        if (ec) return;  // re-armed or cancelled
        if (auto self = weak.lock()) self->on_timer();
    });
}

void Connection::send_close_packet() {
    auto endpoint = endpoint_.lock();
    if (!endpoint || conn_ == nullptr || ngtcp2_conn_in_closing_period2(conn_) ||
        ngtcp2_conn_in_draining_period2(conn_)) {
        return;
    }
    ngtcp2_ccerr ccerr;
    ngtcp2_ccerr_default(&ccerr);
    if (close_tls_alert_ != 0) {
        ngtcp2_ccerr_set_tls_alert(&ccerr, close_tls_alert_, nullptr, 0);
    } else if (close_liberr_ != 0) {
        ngtcp2_ccerr_set_liberr(&ccerr, close_liberr_, nullptr, 0);
    } else {
        ngtcp2_ccerr_set_application_error(&ccerr, close_app_code_, nullptr, 0);
    }

    std::array<std::uint8_t, kMaxUdpPayload> buffer;
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi{};
    auto written = ngtcp2_conn_write_connection_close(conn_, &ps.path, &pi, buffer.data(),
                                                      buffer.size(), &ccerr, now());
    if (written > 0) {
        endpoint->send_packet(
            std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(written))),
            to_endpoint(ps.path.remote, remote_));
    }
}

void Connection::close(std::uint64_t app_error_code) {
    if (closed_) return;
    close_app_code_ = app_error_code;
    // Queued frames go out first: they may carry the closes of relayed connections, which are
    // closed just before the connection to the server.
    if (!tx_datagrams_.empty()) flush();
    if (closed_) return;
    fail(Error{Errc::closed, "connection closed locally"}, true);
}

void Connection::fail(core::Error error, bool send_close) {
    auto self = shared_from_this();
    if (closed_) return;
    if (send_close) send_close_packet();
    closed_ = true;
    timer_.cancel();

    using ConnResult = Result<std::shared_ptr<Connection>>;
    using StreamResult = Result<std::shared_ptr<Stream>>;
    post_completion(executor_, connect_handler_, ConnResult(error));
    post_completion(executor_, accept_handler_, StreamResult(error));
    while (!open_waiters_.empty()) {
        auto handler = std::move(open_waiters_.front());
        open_waiters_.pop_front();
        post_completion(executor_, handler, StreamResult(error));
    }
    auto streams = std::move(streams_);
    streams_.clear();
    for (auto& [id, stream] : streams) stream->on_closed(error);
    incoming_.clear();
    send_queue_.clear();
    rx_datagrams_.clear();
    tx_datagrams_.clear();
    datagram_handler_ = nullptr;

    release_pending_handshake();
    if (auto endpoint = endpoint_.lock()) endpoint->forget(*this);
}

void Connection::release_pending_handshake() {
    if (!pending_handshake_) return;
    pending_handshake_ = false;
    if (auto endpoint = endpoint_.lock()) --endpoint->pending_handshakes_;
}

}  // namespace ghostdrive::transport
