#pragma once

#include <array>
#include <asio/any_completion_handler.hpp>
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/transport/impairment.hpp"

struct ngtcp2_conn;
struct ngtcp2_crypto_conn_ref;
struct ngtcp2_crypto_ossl_ctx;
struct ssl_ctx_st;

namespace ghostdrive::transport {

class Connection;
class Endpoint;
struct Callbacks;

using core::Result;
using core::Status;
using Udp = asio::ip::udp;

// One bidirectional QUIC stream. Not thread-safe: use it only from the endpoint's executor.
class Stream {
public:
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    ~Stream();

    std::int64_t id() const noexcept { return id_; }

    // Reads at least one byte into `out` and returns the count, or 0 at end of stream.
    // One read may be outstanding at a time.
    asio::awaitable<Result<std::size_t>> read(std::span<std::byte> out);

    // Sends the buffers in order, and FIN after them if `fin` is set. Completes once the peer
    // has acknowledged every byte, so the buffers only need to live until then and are never
    // copied. One write may be outstanding at a time.
    asio::awaitable<Status> write(std::span<const std::span<const std::byte>> buffers,
                                  bool fin = false);
    asio::awaitable<Status> write(std::span<const std::byte> data, bool fin = false);

    // Abandons the stream in both directions (RESET_STREAM and STOP_SENDING).
    void reset(std::uint64_t app_error_code);

private:
    friend class Connection;
    friend struct Callbacks;

    using ReadHandler = asio::any_completion_handler<void(Result<std::size_t>)>;
    using WriteHandler = asio::any_completion_handler<void(Status)>;

    struct Private {};

public:
    Stream(Private, std::weak_ptr<Connection> conn, asio::any_io_executor executor,
           std::int64_t id);

private:
    void start_read(ReadHandler handler, std::span<std::byte> out);
    void start_write(WriteHandler handler, std::span<const std::span<const std::byte>> buffers,
                     bool fin);

    // Called by Connection; never resume a coroutine directly (completions are posted).
    void on_data(std::span<const std::byte> data, bool fin);
    void on_acked(std::uint64_t offset, std::uint64_t length);
    void on_sent(std::size_t length, bool fin_flagged);
    void on_closed(core::Error reason);
    bool has_unsent() const noexcept;
    std::size_t unsent_vectors(std::span<std::span<const std::byte>> out) const;
    bool wants_fin() const noexcept { return fin_pending_ && !fin_sent_; }
    void give_credit(std::size_t length);
    void maybe_complete_write();

    std::weak_ptr<Connection> conn_;
    asio::any_io_executor executor_;
    std::int64_t id_;

    // Receive side. Bytes arrive in order; anything not taken by a pending read is buffered.
    core::Bytes rx_;
    std::size_t rx_head_ = 0;
    bool rx_fin_ = false;
    ReadHandler read_handler_;
    std::span<std::byte> read_out_;

    // Send side. Offsets count bytes on the stream since it opened.
    std::vector<std::span<const std::byte>> tx_buffers_;
    std::uint64_t tx_base_ = 0;   // stream offset of tx_buffers_[0][0]
    std::uint64_t tx_end_ = 0;    // stream offset after the last queued byte
    std::uint64_t tx_sent_ = 0;   // bytes handed to ngtcp2
    std::uint64_t tx_acked_ = 0;  // bytes acknowledged by the peer
    bool fin_pending_ = false;
    bool fin_sent_ = false;
    WriteHandler write_handler_;

    std::optional<core::Error> error_;
};

// Who a connection is allowed to talk to. Peers are authenticated only by pinned Ed25519 keys.
using PinCheck = std::function<bool(const core::PublicKey&)>;

// Relay frames are QUIC DATAGRAM frames (RFC 9221) on the authenticated connection to the
// rendezvous server: a 32-byte peer key (the destination when a client sends, the source when
// the server delivers), then one QUIC packet of the relayed connection. Relayed connections send
// packets of at most kMaxRelayedPacket bytes so a frame always fits in one packet of the server
// connection (ARCHITECTURE.md section 3.5).
inline constexpr std::size_t kRelayKeySize = 32;
inline constexpr std::size_t kMaxRelayedPacket = 1280;
inline constexpr std::size_t kMaxRelayFrame = kRelayKeySize + kMaxRelayedPacket;

struct EndpointOptions {
    Udp::endpoint bind{asio::ip::address_v4::loopback(), 0};
    // Incoming connections are accepted only from keys this returns true for. Empty rejects all.
    // Called only after the peer proved it holds the key; an exception counts as a rejection.
    PinCheck accept_peer;
    // Incoming handshakes in progress beyond this are dropped, which bounds what unauthenticated
    // senders can make us allocate.
    std::size_t max_pending_handshakes = 256;
    std::chrono::milliseconds idle_timeout{30'000};
    std::chrono::milliseconds handshake_timeout{10'000};
    // Sends a keep-alive after this much idle time (0: never), which also keeps NAT mappings
    // for the connection open.
    std::chrono::milliseconds keep_alive{0};
    // Servers on the open internet: answer a new Initial with a Retry and create state only
    // when the client returns the token, proving it receives at its address (RFC 9000 8.1).
    // Stops spoofed Initials from filling handshake slots or reflecting handshakes.
    bool require_address_validation = false;
    // Incoming connections accept and may send DATAGRAM frames (the rendezvous server).
    bool accept_datagrams = false;
    Impairment impairment{};
};

struct ConnectOptions {
    // Negotiate DATAGRAM frames (RFC 9221), as the connection to a rendezvous server does.
    bool datagrams = false;
};

// A QUIC connection to one authenticated peer. Not thread-safe: use it only from the
// endpoint's executor.
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    // The peer's pinned key. Valid once connect() or accept() has returned the connection.
    const core::PublicKey& peer_key() const noexcept { return peer_key_; }
    // The address the connection was created with. It does not follow NAT rebinding; use
    // current_remote_endpoint() for where packets go now.
    const Udp::endpoint& remote_endpoint() const noexcept { return remote_; }
    Udp::endpoint current_remote_endpoint() const;
    bool is_open() const noexcept { return !closed_; }

    // Opens a bidirectional stream, waiting for stream credit from the peer if needed.
    asio::awaitable<Result<std::shared_ptr<Stream>>> open_stream();
    // Waits for the peer to open a stream.
    asio::awaitable<Result<std::shared_ptr<Stream>>> accept_stream();

    // Sends CONNECTION_CLOSE and fails all pending operations with Errc::closed.
    void close(std::uint64_t app_error_code = 0);

    // DATAGRAM frames, on connections made with ConnectOptions::datagrams or
    // EndpointOptions::accept_datagrams. The handler runs once ngtcp2 has returned, never inside
    // its callbacks, and only after the handshake.
    using DatagramHandler = std::function<void(core::Bytes datagram)>;
    void set_datagram_handler(DatagramHandler handler) { datagram_handler_ = std::move(handler); }
    // Queues one DATAGRAM frame, sent when congestion control allows. Returns false if it was
    // dropped instead: datagrams are off, it is larger than kMaxDatagram, or the queue is full.
    bool send_datagram(core::Bytes datagram);
    static constexpr std::size_t kMaxDatagram = kMaxRelayFrame;

private:
    friend class Endpoint;
    friend class Stream;
    friend struct Callbacks;

    using ConnectHandler = asio::any_completion_handler<void(Result<std::shared_ptr<Connection>>)>;
    using StreamHandler = asio::any_completion_handler<void(Result<std::shared_ptr<Stream>>)>;

    struct Private {};

    static Result<std::shared_ptr<Connection>> create_client(
        const std::shared_ptr<Endpoint>& endpoint, const Udp::endpoint& remote,
        const core::PublicKey& expected_peer, ConnectOptions options);
    // `original_dcid` is set when the Initial carried a valid Retry token; it is the DCID the
    // client used before the Retry.
    static Result<std::shared_ptr<Connection>> create_server(
        const std::shared_ptr<Endpoint>& endpoint, const Udp::endpoint& remote,
        std::span<const std::uint8_t> initial_packet,
        const std::optional<std::vector<std::uint8_t>>& original_dcid = std::nullopt);

public:
    Connection(Private, const std::shared_ptr<Endpoint>& endpoint, const Udp::endpoint& remote,
               bool is_client);

private:
    Status init_tls(ssl_ctx_st* ssl_ctx);

    void on_packet(std::span<const std::uint8_t> packet, const Udp::endpoint& from);
    void on_timer();
    void flush();
    void schedule_flush();
    void arm_timer();
    void after_events();

    // Fails everything and unregisters from the endpoint. With `send_close`, sends
    // CONNECTION_CLOSE carrying the given library or application error first.
    void fail(core::Error error, bool send_close);
    void release_pending_handshake();
    void send_close_packet();

    bool authorize(const core::PublicKey& key) const;
    Status check_peer_certificate();

    void deliver_datagrams();

    void start_open_stream(StreamHandler handler);
    void start_accept_stream(StreamHandler handler);
    std::shared_ptr<Stream> add_stream(std::int64_t id);
    void enqueue_send(std::int64_t id);

    std::weak_ptr<Endpoint> endpoint_;
    asio::any_io_executor executor_;
    Udp::endpoint local_;
    Udp::endpoint remote_;
    bool is_client_;

    ngtcp2_conn* conn_ = nullptr;
    ngtcp2_crypto_ossl_ctx* ossl_ctx_ = nullptr;
    std::unique_ptr<ngtcp2_crypto_conn_ref> conn_ref_;
    asio::steady_timer timer_;

    std::optional<core::PublicKey> expected_peer_;  // client side
    core::PublicKey peer_key_{};
    std::vector<std::string> cids_;  // our connection IDs registered with the endpoint

    std::unordered_map<std::int64_t, std::shared_ptr<Stream>> streams_;
    std::deque<std::int64_t> send_queue_;
    std::deque<std::shared_ptr<Stream>> incoming_;
    std::deque<StreamHandler> open_waiters_;
    StreamHandler accept_handler_;
    ConnectHandler connect_handler_;

    // Set inside ngtcp2 callbacks, handled in after_events() once ngtcp2 has returned.
    bool handshake_done_ = false;
    bool announced_ = false;
    bool streams_credit_ = false;
    std::optional<core::Error> callback_error_;

    bool datagrams_ = false;
    std::deque<core::Bytes> rx_datagrams_;  // received, not yet handed to the handler
    std::deque<core::Bytes> tx_datagrams_;  // queued for sending
    bool datagram_turn_taken_ = false;      // the last packet sent was a DATAGRAM frame
    DatagramHandler datagram_handler_;

    // Pending close: either a library error code or an application error code.
    int close_liberr_ = 0;
    std::uint64_t close_app_code_ = 0;
    std::uint8_t close_tls_alert_ = 0;

    bool flush_scheduled_ = false;
    bool pending_handshake_ = false;  // counted in Endpoint::pending_handshakes_
    bool closed_ = false;
};

// Owns a UDP socket and the QUIC connections on it. Create with Endpoint::create and run it on
// one executor: a single-threaded io_context, or a strand for a multi-threaded one. Every
// public function must be called from that executor.
class Endpoint : public std::enable_shared_from_this<Endpoint> {
public:
    using DatagramHandler =
        std::function<void(std::span<const std::byte> datagram, const Udp::endpoint& from)>;

    static Result<std::shared_ptr<Endpoint>> create(asio::any_io_executor executor,
                                                    const core::DeviceKey& key,
                                                    EndpointOptions options);
    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;
    ~Endpoint();

    Udp::endpoint local_endpoint() const;
    const core::PublicKey& public_key() const noexcept { return public_key_; }

    // Dials `remote` and completes after a handshake in which the peer proved `peer_key`.
    asio::awaitable<Result<std::shared_ptr<Connection>>> connect(Udp::endpoint remote,
                                                                 core::PublicKey peer_key,
                                                                 ConnectOptions options = {});
    // Waits for an incoming connection whose peer passed EndpointOptions::accept_peer.
    asio::awaitable<Result<std::shared_ptr<Connection>>> accept();

    // Receives datagrams that start with kDatagramMagic (hole-punching probes).
    void set_datagram_handler(DatagramHandler handler) { datagram_handler_ = std::move(handler); }
    // Sends a raw datagram on the shared socket. Subject to the impairment shim.
    void send_datagram(std::span<const std::byte> datagram, const Udp::endpoint& to);

    // Relaying through a rendezvous server. Each relayed peer gets a virtual address (in the
    // unrouted 198.18.0.0/15 range); QUIC dials and talks to it like any address, and the
    // endpoint carries those packets as relay frames on `server`, a connection to the
    // rendezvous server made with datagrams enabled, and hands received frames to QUIC as
    // coming from the source's virtual address. Relayed packets are accepted only from keys
    // `allowed` returns true for.
    void set_relay(const std::shared_ptr<Connection>& server, PinCheck allowed);
    void clear_relay();
    // The virtual address of a relayed peer; nullopt once the table is full.
    std::optional<Udp::endpoint> relay_address(const core::PublicKey& peer);
    bool is_relay_address(const Udp::endpoint& address) const {
        return relay_peer_.contains(address);
    }

    // Closes every connection and the socket. Pending operations fail with Errc::closed.
    void close();

private:
    friend class Connection;
    friend struct Callbacks;

    using ConnectionHandler =
        asio::any_completion_handler<void(Result<std::shared_ptr<Connection>>)>;
    struct Private {};
    struct SslCtxDeleter {
        void operator()(ssl_ctx_st* ctx) const noexcept;
    };
    using SslCtx = std::unique_ptr<ssl_ctx_st, SslCtxDeleter>;

public:
    Endpoint(Private, asio::any_io_executor executor, EndpointOptions options);

private:
    void start_receive();
    void on_receive(std::size_t length);
    void route_quic(std::span<const std::uint8_t> packet, const Udp::endpoint& from);
    void on_relay_frame(std::span<const std::byte> frame);
    void send_retry(std::span<const std::uint8_t> packet, const Udp::endpoint& to);

    void start_connect(ConnectionHandler handler, const Udp::endpoint& remote,
                       const core::PublicKey& peer_key, ConnectOptions options);
    void start_accept(ConnectionHandler handler);
    void on_established(const std::shared_ptr<Connection>& conn);

    void send_packet(std::span<const std::byte> packet, const Udp::endpoint& to);
    void send_now(std::span<const std::byte> packet, const Udp::endpoint& to);

    void register_cid(std::span<const std::uint8_t> cid, const std::shared_ptr<Connection>& conn);
    void unregister_cid(std::span<const std::uint8_t> cid);
    void forget(Connection& conn);

    asio::any_io_executor executor_;
    EndpointOptions options_;
    core::PublicKey public_key_{};
    SslCtx client_ctx_;
    SslCtx server_ctx_;
    std::array<std::uint8_t, 32> static_secret_{};  // keys stateless reset tokens

    std::unordered_map<std::string, std::shared_ptr<Connection>> by_cid_;
    std::size_t pending_handshakes_ = 0;
    std::deque<std::shared_ptr<Connection>> accepted_;
    ConnectionHandler accept_handler_;
    DatagramHandler datagram_handler_;
    std::weak_ptr<Connection> relay_;
    PinCheck relay_allowed_;
    std::map<core::PublicKey, Udp::endpoint> relay_address_;
    std::map<Udp::endpoint, core::PublicKey> relay_peer_;

    std::optional<Impairer> impairer_;

    // The receive buffer outlives the socket member, which is destroyed first.
    std::vector<std::uint8_t> rx_buffer_;
    Udp::endpoint rx_from_;
    Udp::socket socket_;
    bool closed_ = false;
};

}  // namespace ghostdrive::transport
