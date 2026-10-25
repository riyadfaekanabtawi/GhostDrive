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

// A Register with every admission ID and hidden group.
constexpr std::uint32_t kMaxControlFrame = 512;
// Frames waiting for a client that does not read; beyond this they are dropped, not buffered.
constexpr std::size_t kMaxOutbox = 64;
constexpr std::chrono::seconds kRegisterTimeout{10};
// Connect requests per client per window; each one makes the server message two devices.
constexpr int kMaxConnectsPerWindow = 30;
constexpr std::chrono::seconds kConnectWindow{10};
// Keys a client asked for with Connect that may relay to it where it is hidden; the oldest is
// forgotten first.
constexpr std::size_t kMaxAskedKeys = 64;

// The key the per-IP cap counts against: the address itself for IPv4, its /64 for IPv6.
std::string ip_bucket(const asio::ip::address& address) {
    if (address.is_v6() && !address.to_v6().is_v4_mapped()) {
        auto bytes = address.to_v6().to_bytes();
        std::fill(bytes.begin() + 8, bytes.end(), 0);
        return asio::ip::address_v6(bytes).to_string() + "/64";
    }
    return address.to_string();
}

// A token bucket in bytes; starts full.
struct Bucket {
    double tokens = 0;
    std::chrono::steady_clock::time_point refilled{};

    Bucket() = default;
    explicit Bucket(std::size_t burst)
        : tokens(static_cast<double>(burst)), refilled(std::chrono::steady_clock::now()) {}

    void refill(std::chrono::steady_clock::time_point now, std::size_t rate, std::size_t burst) {
        double elapsed = std::chrono::duration<double>(now - refilled).count();
        refilled = now;
        tokens = std::min(static_cast<double>(burst), tokens + elapsed * static_cast<double>(rate));
    }
};

}  // namespace

struct Server::Client {
    core::PublicKey key{};
    std::shared_ptr<transport::Connection> conn;
    std::shared_ptr<transport::Stream> control;
    discovery::GroupId group{};           // where others find it
    std::set<discovery::GroupId> hidden;  // where it may only look for others
    bool placed = false;                  // counted in group_sizes_
    bool registered = false;

    std::deque<core::Bytes> outbox;
    bool writing = false;

    Bucket sent;      // relay frames from this client
    Bucket received;  // relay frames to this client
    std::chrono::steady_clock::time_point connect_window{};
    int connects = 0;
    // Peers it sent Connect for, and when. Kept across a new Register, so relayed connections
    // survive a change of discovery key.
    std::map<core::PublicKey, std::chrono::steady_clock::time_point> asked;

    void ask(const core::PublicKey& peer, std::chrono::steady_clock::time_point now) {
        if (!asked.contains(peer) && asked.size() >= kMaxAskedKeys) {
            asked.erase(std::min_element(asked.begin(), asked.end(), [](const auto& x,
                                                                        const auto& y) {
                return x.second < y.second;
            }));
        }
        asked[peer] = now;
    }
    bool has_asked(const core::PublicKey& peer, std::chrono::steady_clock::time_point now,
                   std::chrono::milliseconds ttl) const {
        auto it = asked.find(peer);
        return it != asked.end() && now - it->second <= ttl;
    }
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
    // Relay frames travel as DATAGRAM frames on these connections; raw UDP is never relayed.
    endpoint_options.accept_datagrams = true;
    auto endpoint = transport::Endpoint::create(executor, key, endpoint_options);
    if (!endpoint) return endpoint.error();
    server->endpoint_ = std::move(endpoint).value();
    asio::co_spawn(executor, server->accept_loop(server->alive_, server->endpoint_),
                   asio::detached);
    return server;
}

std::uint16_t Server::port() const {
    return endpoint_ ? endpoint_->local_endpoint().port() : 0;
}

void Server::stop() {
    for (auto& [key, client] : clients_) client->conn->close();
    clients_.clear();
    if (endpoint_) endpoint_->close();
}

// The guard and endpoint are parameters, copied into the frame at the call: the body first runs
// on the executor, possibly after the server is gone, so it must not read members before the
// alive check.
asio::awaitable<void> Server::accept_loop(std::shared_ptr<bool> alive,
                                          std::shared_ptr<transport::Endpoint> endpoint) {
    for (;;) {
        if (!*alive) co_return;
        auto conn = co_await endpoint->accept();
        if (!*alive || !conn) co_return;
        auto ip = ip_bucket(conn.value()->remote_endpoint().address());
        if (per_ip_[ip] >= options_.max_clients_per_ip || clients_.size() >= options_.max_clients) {
            conn.value()->close();
            continue;
        }
        ++per_ip_[ip];
        asio::co_spawn(executor_, serve(alive, std::move(conn).value()), asio::detached);
    }
}

// Spawned, so it too may first run after the server is gone.
asio::awaitable<void> Server::serve(std::shared_ptr<bool> alive,
                                    std::shared_ptr<transport::Connection> conn) {
    if (!*alive) {
        conn->close();
        co_return;
    }
    auto ip = ip_bucket(conn->remote_endpoint().address());
    auto client = std::make_shared<Client>();
    client->key = conn->peer_key();
    client->conn = conn;
    client->sent = Bucket(options_.relay_burst_bytes);
    client->received = Bucket(options_.relay_receive_burst_bytes);
    // The connection authenticates the sender, so a frame needs no token and shows nothing
    // identifying on the wire. Weak: the connection owns this handler.
    conn->set_datagram_handler(
        [this, alive, weak = std::weak_ptr<Client>(client)](core::Bytes frame) {
            auto sender = weak.lock();
            if (*alive && sender) on_relay_frame(*sender, std::move(frame));
        });

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
        if (reg && admits(*reg)) {
            place(client, *reg);
            client->registered = true;
            clients_[client->key] = client;
            stats_.clients = clients_.size();
            deadline.cancel();
            send(client, discovery::encode(discovery::Registered{conn->current_remote_endpoint()}));

            for (;;) {
                auto frame = co_await wire::read_frame(*client->control, kMaxControlFrame);
                if (!*alive) co_return;
                if (frame && frame.value().type == wire::FrameType::rendezvous_register) {
                    // Moving to other groups (the drive's discovery key changed); the
                    // connection, and so any relayed connection, stays.
                    auto again = discovery::decode_register(frame.value().payload);
                    if (!again) break;
                    unplace(client);
                    if (!admits(again.value())) break;  // its old groups were released above
                    place(client, again.value());
                    send(client,
                         discovery::encode(discovery::Registered{conn->current_remote_endpoint()}));
                    continue;
                }
                if (!frame || frame.value().type != wire::FrameType::rendezvous_connect) break;
                auto request = discovery::decode_connect(frame.value().payload);
                if (!request) break;
                auto now = std::chrono::steady_clock::now();
                // Counted even when over the limit below: it only opts this client in to
                // relay frames from that key.
                client->ask(request.value().peer, now);
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
    const auto& allowed = options_.allowed_groups;
    if (!allowed.empty() &&
        std::none_of(reg.admissions.begin(), reg.admissions.end(),
                     [&](const discovery::GroupId& id) { return allowed.contains(id); })) {
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

void Server::on_relay_frame(Client& sender, core::Bytes frame) {
    // The sender is the authenticated client on whose connection the frame arrived; until it
    // registers (or once it is replaced or moving between groups) it may not relay.
    if (!sender.registered || !sender.placed || frame.size() <= transport::kRelayKeySize ||
        frame.size() > transport::kMaxRelayFrame ||
        transport::classify_packet(std::span(frame).subspan(transport::kRelayKeySize)) !=
            transport::PacketKind::quic) {
        ++stats_.dropped_datagrams;
        return;
    }
    core::PublicKey dest_key{};
    std::copy_n(frame.begin(), transport::kRelayKeySize, dest_key.begin());
    // Between clients in one group, or where one is hidden in the other's group and asked for
    // it with Connect recently. Anyone who knows an earlier key (a removed device, or anyone on
    // an open server) can register visibly in its group, so without the second condition it
    // could use up the receive budget of every member hidden there. The hidden side's ask
    // covers both directions of the connection.
    auto now = std::chrono::steady_clock::now();
    auto dest = clients_.find(dest_key);
    auto may_relay = [&](const Client& a, const Client& b) {
        if (a.group == b.group) return true;
        auto ttl = options_.asked_ttl;
        return (a.hidden.contains(b.group) && a.has_asked(b.key, now, ttl)) ||
               (b.hidden.contains(a.group) && b.has_asked(a.key, now, ttl));
    };
    if (dest == clients_.end() || dest->second.get() == &sender || !dest->second->placed ||
        !may_relay(sender, *dest->second)) {
        ++stats_.dropped_datagrams;
        return;
    }

    auto& receiver = *dest->second;
    sender.sent.refill(now, options_.relay_bytes_per_second, options_.relay_burst_bytes);
    receiver.received.refill(now, options_.relay_receive_bytes_per_second,
                             options_.relay_receive_burst_bytes);
    auto size = frame.size();
    auto cost = static_cast<double>(size);
    if (sender.sent.tokens < cost || receiver.received.tokens < cost) {
        ++stats_.dropped_datagrams;  // over budget: drop, never queue (QUIC recovers)
        return;
    }
    sender.sent.tokens -= cost;
    receiver.received.tokens -= cost;

    // The frame is reused for the receiver: the destination key becomes the source key, and
    // the packet bytes stay where they are.
    std::copy(sender.key.begin(), sender.key.end(), frame.begin());
    // A receiver whose queue is full (it is not keeping up) loses the frame.
    if (!receiver.conn->send_datagram(std::move(frame))) {
        ++stats_.dropped_datagrams;
        return;
    }
    ++stats_.relayed_datagrams;
    stats_.relayed_bytes += size;
}

}  // namespace ghostdrive::rendezvous
