#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/udp.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <memory>

#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/discovery/rendezvous_codec.hpp"
#include "ghostdrive/transport/endpoint.hpp"

namespace ghostdrive::rendezvous {

inline constexpr std::uint16_t kDefaultPort = 7374;

struct ServerOptions {
    asio::ip::udp::endpoint listen{asio::ip::address_v4::any(), kDefaultPort};
    std::size_t max_clients = 10'000;
    // Per IPv4 address, or per IPv6 /64 (a single host usually holds a whole /64).
    std::size_t max_clients_per_ip = 16;
    std::size_t max_group_size = 256;
    // When not empty, only drives with these admission IDs may register: an operator serving
    // only their own drives lists them here (`ghostdrive status` shows a drive's ID as its
    // group).
    std::set<discovery::GroupId> allowed_groups;
    // Relay budget per client, as a token bucket.
    std::size_t relay_bytes_per_second = 4u * 1024 * 1024;
    std::size_t relay_burst_bytes = 1024 * 1024;
    transport::Impairment impairment{};
};

struct ServerStats {
    std::uint64_t clients = 0;
    std::uint64_t relayed_datagrams = 0;
    std::uint64_t relayed_bytes = 0;
    std::uint64_t dropped_datagrams = 0;
};

// Self-hostable rendezvous and relay (ARCHITECTURE.md section 3.5). Devices hold a QUIC
// connection to it; it tells members of a group where the others are, and forwards relay
// datagrams between members of one group. It keeps everything in memory and never sees file
// data or keys beyond the devices' public keys: relayed packets are QUIC, end-to-end encrypted
// between the devices.
//
// Unlike a GhostDrive peer, the server completes TLS with any client key (the handshake still
// proves the client holds it); a client gets service only after registering into a group, and
// only members of a drive can compute its group ID. Use from one executor.
class Server {
public:
    static core::Result<std::unique_ptr<Server>> start(asio::any_io_executor executor,
                                                       const core::DeviceKey& key,
                                                       ServerOptions options = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const;
    const ServerStats& stats() const noexcept { return stats_; }
    void stop();

    struct Private {};
    Server(Private, asio::any_io_executor executor, ServerOptions options);

private:
    struct Client;

    asio::awaitable<void> accept_loop();
    asio::awaitable<void> serve(std::shared_ptr<transport::Connection> conn);
    void send(const std::shared_ptr<Client>& client, core::Bytes frame);
    void on_datagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& from);
    void drop(const std::shared_ptr<Client>& client);
    // Whether `reg` may register: admitted by the allow list, and every group in it has room.
    bool admits(const discovery::Register& reg);
    void place(const std::shared_ptr<Client>& client, const discovery::Register& reg);
    void unplace(const std::shared_ptr<Client>& client);

    asio::any_io_executor executor_;
    ServerOptions options_;
    std::shared_ptr<transport::Endpoint> endpoint_;
    std::map<core::PublicKey, std::shared_ptr<Client>> clients_;
    std::map<discovery::RelayToken, std::shared_ptr<Client>> by_token_;
    std::map<std::string, std::size_t> per_ip_;
    std::map<discovery::GroupId, std::size_t> group_sizes_;
    ServerStats stats_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace ghostdrive::rendezvous
