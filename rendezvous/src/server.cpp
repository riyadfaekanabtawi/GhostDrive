#include "ghostdrive/rendezvous/server.hpp"

#include <algorithm>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>

#include "ghostdrive/transport/demux.hpp"
#include "ghostdrive/transport/frame.hpp"

namespace ghostdrive::rendezvous {

using core::Errc;
using core::Error;
namespace wire = transport::wire;
using Udp = asio::ip::udp;

namespace {

constexpr std::uint32_t kMaxControlFrame = 256;  // a Register with every hidden group
// Frames waiting for a client that does not read; beyond this they are dropped, not buffered.
constexpr std::size_t kMaxOutbox = 64;
constexpr std::chrono::seconds kRegisterTimeout{10};
// Clients send QUIC packets of at most 1400 bytes; with the relay header that still fits a
// 1500-byte MTU.
constexpr std::size_t kMaxRelayPayload = 1400;
// Connect requests per client per window; each one makes the server message two devices.
constexpr int kMaxConnectsPerWindow = 30;
constexpr std::chrono::seconds kConnectWindow{10};

// The key the per-IP cap counts against: the address itself for IPv4, its /64 for IPv6.
std::string ip_bucket(const asio::ip::address& address) {
    if (address.is_v6() && !address.to_v6().is_v4_mapped()) {
        auto bytes = address.to_v6().to_bytes();
        std::fill(bytes.begin() + 8, bytes.end(), 0);
        return asio::ip::address_v6(bytes).to_string() + "/64";
    }
    return address.to_string();
}

}  // namespace

struct Server::Client {
    core::PublicKey key{};
    std::shared_ptr<transport::Connection> conn;
    std::shared_ptr<transport::Stream> control;
    discovery::GroupId group{};           // where others find it
    std::set<discovery::GroupId> hidden;  // where it may only look for others
    bool placed = false;                  // counted in group_sizes_
    discovery::RelayToken token{};
    bool registered = false;

    std::deque<core::Bytes> outbox;
    bool writing = false;

    double tokens = 0;
    std::chrono::steady_clock::time_point refilled{};
    std::chrono::steady_clock::time_point connect_window{};
    int connects = 0;
};

Server::Server(Private, asio::any_io_executor executor, ServerOptions options)
    : executor_(std::move(executor)), options_(std::move(options)) {}

Server::~Server() {
    *alive_ = false;
    if (endpoint_) endpoint_->close();
}

core::Result<std::unique_ptr<Server>> Server::start(asio::any_io_executor executor,
                                                    const core::DeviceKey& key,
                                                    ServerOptions options) {
    auto server = std::make_unique<Server>(Private{}, executor, std::move(options));
    transport::EndpointOptions endpoint_options;
    endpoint_options.bind = server->options_.listen;
    endpoint_options.impairment = server->options_.impairment;
    endpoint_options.keep_alive = std::chrono::seconds(15);
    endpoint_options.idle_timeout = std::chrono::seconds(60);
    endpoint_options.handshake_timeout = std::chrono::seconds(5);
    // Spoofed Initials get a Retry and no state, so they can neither fill handshake slots nor
    // make the server send its handshake flight to someone else.
    endpoint_options.require_address_validation = true;
    // The one place that completes TLS with keys it has not pinned: devices are not known to
    // the server in advance. The handshake still proves each client holds its key, and nothing
    // is served before the client registers into a group (see serve()).
    endpoint_options.accept_peer = [](const core::PublicKey&) { return true; };
    auto endpoint = transport::Endpoint::create(executor, key, endpoint_options);
    if (!endpoint) return endpoint.error();
    server->endpoint_ = std::move(endpoint).value();
    auto* raw = server.get();
    server->endpoint_->set_datagram_handler(
        [raw, alive = server->alive_](std::span<const std::byte> datagram,
                                      const Udp::endpoint& from) {
            if (*alive) raw->on_datagram(datagram, from);
        });
    asio::co_spawn(executor, server->accept_loop(), asio::detached);
    return server;
}

std::uint16_t Server::port() const {
    return endpoint_ ? endpoint_->local_endpoint().port() : 0;
}

void Server::stop() {
    for (auto& [key, client] : clients_) client->conn->close();
    clients_.clear();
    by_token_.clear();
    if (endpoint_) endpoint_->close();
}

asio::awaitable<void> Server::accept_loop() {
    auto alive = alive_;
    auto endpoint = endpoint_;
    for (;;) {
        auto conn = co_await endpoint->accept();
        if (!*alive || !conn) co_return;
        auto ip = ip_bucket(conn.value()->remote_endpoint().address());
        if (per_ip_[ip] >= options_.max_clients_per_ip || clients_.size() >= options_.max_clients) {
            conn.value()->close();
            continue;
        }
        ++per_ip_[ip];
        asio::co_spawn(executor_, serve(std::move(conn).value()), asio::detached);
    }
}

asio::awaitable<void> Server::serve(std::shared_ptr<transport::Connection> conn) {
    auto alive = alive_;
    auto ip = ip_bucket(conn->remote_endpoint().address());
    auto client = std::make_shared<Client>();
    client->key = conn->peer_key();
    client->conn = conn;
    client->tokens = static_cast<double>(options_.relay_burst_bytes);
    client->refilled = std::chrono::steady_clock::now();

    // Registration must come promptly; until then the client gets nothing.
    asio::steady_timer deadline(executor_, kRegisterTimeout);
    deadline.async_wait([conn, client](const std::error_code& ec) {
        if (!ec && !client->registered) conn->close();
    });

    auto stream = co_await conn->accept_stream();
    if (!*alive) co_return;
    if (stream) {
        client->control = stream.value();
        auto first = co_await wire::read_frame(*client->control, kMaxControlFrame);
        if (!*alive) co_return;
        std::optional<discovery::Register> reg;
        if (first && first.value().type == wire::FrameType::rendezvous_register) {
            auto decoded = discovery::decode_register(first.value().payload);
            if (decoded) reg = decoded.value();
        }
        // A device that reconnects replaces its old registration, which must not count against
        // the room it needs.
        if (reg) {
            if (auto it = clients_.find(client->key); it != clients_.end()) drop(it->second);
        }
        if (reg && admits(*reg) && core::random_bytes(client->token)) {
            place(client, *reg);
            client->registered = true;
            clients_[client->key] = client;
            by_token_[client->token] = client;
            stats_.clients = clients_.size();
            deadline.cancel();
            send(client, discovery::encode(discovery::Registered{conn->current_remote_endpoint(),
                                                                 client->token}));

            for (;;) {
                auto frame = co_await wire::read_frame(*client->control, kMaxControlFrame);
                if (!*alive) co_return;
                if (frame && frame.value().type == wire::FrameType::rendezvous_register) {
                    // Moving to other groups (the drive's discovery key changed); the relay
                    // token, and so any relayed connection, stays.
                    auto again = discovery::decode_register(frame.value().payload);
                    if (!again) break;
                    unplace(client);
                    if (!admits(again.value())) break;  // its old groups were released above
                    place(client, again.value());
                    send(client, discovery::encode(discovery::Registered{
                                     conn->current_remote_endpoint(), client->token}));
                    continue;
                }
                if (!frame || frame.value().type != wire::FrameType::rendezvous_connect) break;
                auto request = discovery::decode_connect(frame.value().payload);
                if (!request) break;
                auto now = std::chrono::steady_clock::now();
                if (now - client->connect_window > kConnectWindow) {
                    client->connect_window = now;
                    client->connects = 0;
                }
                if (++client->connects > kMaxConnectsPerWindow) continue;

                // Found only in the group it registered in, by a client there or looking there.
                auto it = clients_.find(request.value().peer);
                if (it == clients_.end() || it->second == client ||
                    (it->second->group != client->group &&
                     !client->hidden.contains(it->second->group))) {
                    send(client,
                         discovery::encode(discovery::PeerAt{request.value().peer, std::nullopt}));
                    continue;
                }
                // Tell both sides at once, so their hole-punching probes cross.
                auto& peer = it->second;
                // Current paths, so NAT rebinding does not leave stale addresses.
                send(client, discovery::encode(
                                 discovery::PeerAt{peer->key, peer->conn->current_remote_endpoint()}));
                send(peer, discovery::encode(
                               discovery::PeerAt{client->key, conn->current_remote_endpoint()}));
            }
        }
    }
    if (*alive) {
        if (auto it = clients_.find(client->key); it != clients_.end() && it->second == client)
            drop(client);
        conn->close();
        if (--per_ip_[ip] == 0) per_ip_.erase(ip);
    }
}

void Server::drop(const std::shared_ptr<Client>& client) {
    if (auto it = by_token_.find(client->token); it != by_token_.end() && it->second == client) {
        by_token_.erase(it);
    }
    if (client->registered) {
        unplace(client);
        client->registered = false;
    }
    if (auto it = clients_.find(client->key); it != clients_.end() && it->second == client) {
        clients_.erase(it);
    }
    stats_.clients = clients_.size();
    client->conn->close();
}

bool Server::admits(const discovery::Register& reg) {
    if (!options_.allowed_groups.empty() && !options_.allowed_groups.contains(reg.admission)) {
        return false;
    }
    // Only the group a client can be found in counts: anyone who knows an earlier group could
    // otherwise fill it and lock every member out.
    auto it = group_sizes_.find(reg.group);
    return it == group_sizes_.end() || it->second < options_.max_group_size;
}

void Server::place(const std::shared_ptr<Client>& client, const discovery::Register& reg) {
    client->group = reg.group;
    client->hidden = std::set<discovery::GroupId>(reg.hidden.begin(), reg.hidden.end());
    client->hidden.erase(reg.group);
    ++group_sizes_[client->group];
    client->placed = true;
}

void Server::unplace(const std::shared_ptr<Client>& client) {
    if (!client->placed) return;
    client->placed = false;
    if (auto it = group_sizes_.find(client->group); it != group_sizes_.end() && --it->second == 0) {
        group_sizes_.erase(it);
    }
    client->hidden.clear();
}

void Server::send(const std::shared_ptr<Client>& client, core::Bytes frame) {
    if (client->outbox.size() >= kMaxOutbox) return;  // the client is not reading; drop
    client->outbox.push_back(std::move(frame));
    if (client->writing || !client->control) return;
    client->writing = true;
    asio::co_spawn(
        executor_,
        [client]() -> asio::awaitable<void> {
            while (!client->outbox.empty()) {
                auto next = std::move(client->outbox.front());
                client->outbox.pop_front();
                if (!co_await client->control->write(next)) {
                    client->outbox.clear();
                    break;
                }
            }
            client->writing = false;
        },
        asio::detached);
}

void Server::on_datagram(std::span<const std::byte> datagram, const Udp::endpoint& from) {
    auto relayed = transport::parse_relay(datagram);
    if (!relayed || relayed->payload.size() > kMaxRelayPayload ||
        transport::classify_packet(relayed->payload) != transport::PacketKind::quic) {
        ++stats_.dropped_datagrams;
        return;
    }
    // The sender is whoever holds this registration token. The source address alone would be
    // spoofable; it must also match the sender's current path, so a token seen on the wire
    // cannot be replayed from elsewhere.
    discovery::RelayToken token{};
    std::copy(relayed->token.begin(), relayed->token.end(), token.begin());
    auto src = by_token_.find(token);
    if (src == by_token_.end() || src->second->conn->current_remote_endpoint() != from) {
        ++stats_.dropped_datagrams;
        return;
    }
    core::PublicKey dest_key{};
    std::copy(relayed->key.begin(), relayed->key.end(), dest_key.begin());
    // Between clients that could have found each other: a shared group, or one looking in the
    // other's group.
    auto dest = clients_.find(dest_key);
    auto shares = [](const Client& a, const Client& b) {
        return a.group == b.group || a.hidden.contains(b.group) || b.hidden.contains(a.group);
    };
    if (dest == clients_.end() || dest->second == src->second ||
        !shares(*src->second, *dest->second)) {
        ++stats_.dropped_datagrams;
        return;
    }

    auto& sender = *src->second;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - sender.refilled).count();
    sender.refilled = now;
    sender.tokens = std::min(static_cast<double>(options_.relay_burst_bytes),
                             sender.tokens + elapsed * static_cast<double>(options_.relay_bytes_per_second));
    if (sender.tokens < static_cast<double>(relayed->payload.size())) {
        ++stats_.dropped_datagrams;  // over budget: drop, never queue (QUIC recovers)
        return;
    }
    sender.tokens -= static_cast<double>(relayed->payload.size());

    // COPY: the relay rewraps the datagram for the receiver: its token, the sender's key (C4).
    auto& receiver = *dest->second;
    auto out = transport::make_relay(receiver.token, sender.key, relayed->payload);
    endpoint_->send_datagram(out, receiver.conn->current_remote_endpoint());
    ++stats_.relayed_datagrams;
    stats_.relayed_bytes += relayed->payload.size();
}

}  // namespace ghostdrive::rendezvous
