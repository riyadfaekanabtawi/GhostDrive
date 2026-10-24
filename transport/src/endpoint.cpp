#include "ghostdrive/transport/endpoint.hpp"

#include <ngtcp2/ngtcp2.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <asio/async_result.hpp>
#include <asio/use_awaitable.hpp>
#include <string>

#include "callbacks.hpp"
#include "completion.hpp"
#include "ghostdrive/transport/demux.hpp"
#include "tls.hpp"

namespace ghostdrive::transport {

using core::Errc;
using core::Error;
using detail::post_completion;

namespace {

constexpr std::size_t kReceiveBufferSize = 65536;
// Matches the connection ID length chosen in connection.cpp; short headers carry no length.
constexpr std::size_t kCidLen = 18;

std::string cid_key(std::span<const std::uint8_t> cid) {
    return std::string(reinterpret_cast<const char*>(cid.data()), cid.size());
}

}  // namespace

void Endpoint::SslCtxDeleter::operator()(ssl_ctx_st* ctx) const noexcept {
    SSL_CTX_free(ctx);
}

Endpoint::Endpoint(Private, asio::any_io_executor executor, EndpointOptions options)
    : executor_(std::move(executor)),
      options_(std::move(options)),
      rx_buffer_(kReceiveBufferSize),
      socket_(executor_) {
    if (options_.impairment.active()) impairer_.emplace(options_.impairment);
}

Result<std::shared_ptr<Endpoint>> Endpoint::create(asio::any_io_executor executor,
                                                   const core::DeviceKey& key,
                                                   EndpointOptions options) {
    auto self = std::make_shared<Endpoint>(Private{}, std::move(executor), std::move(options));
    self->public_key_ = key.public_key();

    auto client_ctx = tls::make_context(key, false, Callbacks::verify_certificate);
    if (!client_ctx) return client_ctx.error();
    auto server_ctx = tls::make_context(key, true, Callbacks::verify_certificate);
    if (!server_ctx) return server_ctx.error();
    self->client_ctx_.reset(client_ctx.value().release());
    self->server_ctx_.reset(server_ctx.value().release());

    if (RAND_bytes(self->static_secret_.data(), static_cast<int>(self->static_secret_.size())) !=
        1) {
        return Error{Errc::crypto, "RAND_bytes failed"};
    }

    std::error_code ec;
    const auto& bind = self->options_.bind;
    self->socket_.open(bind.protocol(), ec);
    if (!ec) self->socket_.bind(bind, ec);
    if (!ec) self->socket_.non_blocking(true, ec);
    if (ec) {
        return Error{Errc::io, "binding UDP socket to " + bind.address().to_string() + ":" +
                                   std::to_string(bind.port()) + ": " + ec.message()};
    }
    self->start_receive();
    return self;
}

Endpoint::~Endpoint() {
    // Connections outlive us only through user references; fail them so nothing waits forever.
    std::vector<std::shared_ptr<Connection>> conns;
    for (auto& [cid, conn] : by_cid_) conns.push_back(conn);
    for (auto& conn : accepted_) conns.push_back(conn);
    by_cid_.clear();
    accepted_.clear();
    for (auto& conn : conns) conn->fail(Error{Errc::closed, "endpoint destroyed"}, false);
    post_completion(executor_, accept_handler_,
                    Result<std::shared_ptr<Connection>>(Error{Errc::closed, "endpoint destroyed"}));
    OPENSSL_cleanse(static_secret_.data(), static_secret_.size());
}

Udp::endpoint Endpoint::local_endpoint() const {
    std::error_code ec;
    auto ep = socket_.local_endpoint(ec);
    return ec ? Udp::endpoint{} : ep;
}

asio::awaitable<Result<std::shared_ptr<Connection>>> Endpoint::connect(Udp::endpoint remote,
                                                                       core::PublicKey peer_key) {
    co_return co_await asio::async_initiate<const asio::use_awaitable_t<>,
                                            void(Result<std::shared_ptr<Connection>>)>(
        [this](auto handler, const Udp::endpoint& to, const core::PublicKey& key) {
            start_connect(ConnectionHandler(std::move(handler)), to, key);
        },
        asio::use_awaitable, remote, peer_key);
}

asio::awaitable<Result<std::shared_ptr<Connection>>> Endpoint::accept() {
    co_return co_await asio::async_initiate<const asio::use_awaitable_t<>,
                                            void(Result<std::shared_ptr<Connection>>)>(
        [this](auto handler) { start_accept(ConnectionHandler(std::move(handler))); },
        asio::use_awaitable);
}

void Endpoint::start_connect(ConnectionHandler handler, const Udp::endpoint& remote,
                             const core::PublicKey& peer_key) {
    using ConnResult = Result<std::shared_ptr<Connection>>;
    if (closed_) {
        post_completion(executor_, handler, ConnResult(Error{Errc::closed, "endpoint closed"}));
        return;
    }
    auto conn = Connection::create_client(shared_from_this(), remote, peer_key);
    if (!conn) {
        post_completion(executor_, handler, ConnResult(conn.error()));
        return;
    }
    conn.value()->connect_handler_ = std::move(handler);
    conn.value()->flush();
}

void Endpoint::start_accept(ConnectionHandler handler) {
    using ConnResult = Result<std::shared_ptr<Connection>>;
    if (accept_handler_) {
        post_completion(executor_, handler,
                        ConnResult(Error{Errc::invalid_argument, "accept already pending"}));
        return;
    }
    while (!accepted_.empty() && !accepted_.front()->is_open()) accepted_.pop_front();
    if (!accepted_.empty()) {
        auto conn = std::move(accepted_.front());
        accepted_.pop_front();
        post_completion(executor_, handler, ConnResult(std::move(conn)));
        return;
    }
    if (closed_) {
        post_completion(executor_, handler, ConnResult(Error{Errc::closed, "endpoint closed"}));
        return;
    }
    accept_handler_ = std::move(handler);
}

void Endpoint::on_established(const std::shared_ptr<Connection>& conn) {
    if (accept_handler_) {
        post_completion(executor_, accept_handler_, Result<std::shared_ptr<Connection>>(conn));
    } else {
        accepted_.push_back(conn);
    }
}

void Endpoint::start_receive() {
    socket_.async_receive_from(
        asio::buffer(rx_buffer_), rx_from_,
        [weak = weak_from_this()](const std::error_code& ec, std::size_t length) {
            auto self = weak.lock();
            if (!self || self->closed_ || ec == asio::error::operation_aborted) return;
            // Other errors (for example ICMP port unreachable reported on the socket) only
            // concern one datagram.
            if (!ec) self->on_receive(length);
            self->start_receive();
        });
}

void Endpoint::on_receive(std::size_t length) {
    auto packet = std::span<const std::uint8_t>(rx_buffer_.data(), length);
    switch (classify_packet(std::as_bytes(packet))) {
        case PacketKind::quic:
            route_quic(packet, rx_from_);
            break;
        case PacketKind::ghostdrive:
            if (datagram_handler_) datagram_handler_(std::as_bytes(packet), rx_from_);
            break;
        case PacketKind::unknown:
            break;
    }
}

void Endpoint::route_quic(std::span<const std::uint8_t> packet, const Udp::endpoint& from) {
    ngtcp2_version_cid vc;
    if (ngtcp2_pkt_decode_version_cid(&vc, packet.data(), packet.size(), kCidLen) != 0) {
        return;  // malformed, or a version we do not speak
    }
    auto it = by_cid_.find(cid_key({vc.dcid, vc.dcidlen}));
    if (it != by_cid_.end()) {
        auto conn = it->second;  // keep it alive even if it unregisters while handling
        conn->on_packet(packet, from);
        return;
    }
    // Only an Initial can start a connection; without a pin check nobody may connect.
    bool long_header = (packet[0] & 0x80) != 0;
    if (!long_header || !options_.accept_peer || closed_) return;
    if (pending_handshakes_ >= options_.max_pending_handshakes) return;
    auto conn = Connection::create_server(shared_from_this(), from, packet);
    if (!conn) return;
    conn.value()->on_packet(packet, from);
}

void Endpoint::send_datagram(std::span<const std::byte> datagram, const Udp::endpoint& to) {
    send_packet(datagram, to);
}

void Endpoint::send_packet(std::span<const std::byte> packet, const Udp::endpoint& to) {
    if (!impairer_) {
        send_now(packet, to);
        return;
    }
    for (auto delay : impairer_->plan()) {
        if (delay.count() == 0) {
            send_now(packet, to);
            continue;
        }
        // COPY: the test impairment shim holds the datagram until its simulated delivery time.
        auto copy = std::make_shared<std::vector<std::byte>>(packet.begin(), packet.end());
        auto timer = std::make_shared<asio::steady_timer>(executor_, delay);
        timer->async_wait([weak = weak_from_this(), timer, copy, to](const std::error_code& ec) {
            if (ec) return;
            if (auto self = weak.lock()) self->send_now(*copy, to);
        });
    }
}

void Endpoint::send_now(std::span<const std::byte> packet, const Udp::endpoint& to) {
    if (!socket_.is_open()) return;
    std::error_code ec;
    // A full socket buffer drops the datagram; QUIC loss recovery resends it.
    socket_.send_to(asio::buffer(packet.data(), packet.size()), to, 0, ec);
}

void Endpoint::register_cid(std::span<const std::uint8_t> cid,
                            const std::shared_ptr<Connection>& conn) {
    auto key = cid_key(cid);
    by_cid_[key] = conn;
    conn->cids_.push_back(std::move(key));
}

void Endpoint::unregister_cid(std::span<const std::uint8_t> cid) {
    by_cid_.erase(cid_key(cid));
}

void Endpoint::forget(Connection& conn) {
    for (const auto& key : conn.cids_) {
        auto it = by_cid_.find(key);
        if (it != by_cid_.end() && it->second.get() == &conn) by_cid_.erase(it);
    }
    conn.cids_.clear();
    std::erase_if(accepted_, [&conn](const auto& c) { return c.get() == &conn; });
}

void Endpoint::close() {
    if (closed_) return;
    closed_ = true;
    std::vector<std::shared_ptr<Connection>> conns;
    for (auto& [cid, conn] : by_cid_) {
        if (std::find(conns.begin(), conns.end(), conn) == conns.end()) conns.push_back(conn);
    }
    for (auto& conn : accepted_) {
        if (std::find(conns.begin(), conns.end(), conn) == conns.end()) conns.push_back(conn);
    }
    for (auto& conn : conns) conn->close();
    accepted_.clear();
    post_completion(executor_, accept_handler_,
                    Result<std::shared_ptr<Connection>>(Error{Errc::closed, "endpoint closed"}));
    std::error_code ec;
    socket_.close(ec);
}

}  // namespace ghostdrive::transport
