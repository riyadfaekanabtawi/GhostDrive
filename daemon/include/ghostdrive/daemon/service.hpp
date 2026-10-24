#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/udp.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ghostdrive/core/chunk_cache.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/daemon/codec.hpp"
#include "ghostdrive/daemon/drive.hpp"
#include "ghostdrive/daemon/invite.hpp"
#include "ghostdrive/transport/endpoint.hpp"
#include "ghostdrive/vfs/fetcher.hpp"
#include "ghostdrive/vfs/filesystem.hpp"
#include "ghostdrive/vfs/tree.hpp"

namespace ghostdrive::discovery {
class LanDiscovery;
struct LanSighting;
}  // namespace ghostdrive::discovery

namespace ghostdrive::daemon {

inline constexpr std::uint16_t kDefaultPort = 7373;

struct ServiceOptions {
    asio::ip::udp::endpoint listen{asio::ip::address_v4::any(), kDefaultPort};
    transport::Impairment impairment{};
    std::chrono::milliseconds sync_poll{250};
    std::chrono::milliseconds redial{2'000};
    std::size_t cache_bytes = 256u * 1024 * 1024;
    // Addresses put in invite codes. Empty: this host's non-loopback IPv4 addresses, then
    // 127.0.0.1.
    std::vector<asio::ip::address> invite_addresses;
    // Advertise this device and find the others on the LAN (mDNS), when built with DNS-SD.
    bool lan_discovery = true;
};

struct PeerStatus {
    core::PublicKey key{};
    std::string name;
    std::string address;
    bool connected = false;
};

struct ServiceStatus {
    std::string device_name;
    std::string fingerprint;
    std::uint16_t port = 0;
    std::vector<PeerStatus> peers;
};

// Runs one device of a drive: the QUIC endpoint, connections to the other members (dialing
// known addresses and accepting pinned keys), chunk serving, index exchange, pairing by invite,
// and the vfs::Filesystem the FUSE mount sits on. Everything runs on `io`; blocking disk and
// SQLite work runs on `disk`. Public functions must be called from `io` (or before it runs).
class Service {
public:
    Service(asio::any_io_executor io, asio::any_io_executor disk, Drive& drive,
            ServiceOptions options = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    core::Status start();
    // Closes every connection and the socket. The filesystem stays usable for local files.
    void stop();

    vfs::Filesystem& filesystem() noexcept { return *fs_; }
    std::uint16_t port() const;

    // A one-time invite code valid for `ttl`.
    core::Result<std::string> create_invite(std::chrono::seconds ttl);
    ServiceStatus status() const;
    bool connected_to(const core::PublicKey& key) const { return live_.contains(key); }

private:
    struct PendingInvite {
        Token token;
        std::chrono::steady_clock::time_point expires;
    };

    bool is_member(const core::PublicKey& key) const;
    bool invites_active();

    asio::awaitable<void> accept_loop();
    asio::awaitable<void> dial_loop();
    asio::awaitable<void> dial(core::PublicKey key, asio::ip::udp::endpoint address);
    asio::awaitable<void> run_member(std::shared_ptr<transport::Connection> conn);
    asio::awaitable<void> handle_stream(std::shared_ptr<transport::Connection> conn,
                                        std::shared_ptr<transport::Stream> stream);
    asio::awaitable<void> push_index(std::shared_ptr<transport::Connection> conn);
    asio::awaitable<void> receive_index(std::shared_ptr<transport::Connection> conn,
                                        std::shared_ptr<transport::Stream> stream, Hello hello);
    asio::awaitable<void> run_pairing(std::shared_ptr<transport::Connection> conn);
    void on_lan_sighting(const discovery::LanSighting& sighting);

    asio::any_io_executor io_;
    asio::any_io_executor disk_;
    Drive& drive_;
    ServiceOptions options_;

    core::ChunkCache cache_;
    vfs::Tree tree_;
    std::unique_ptr<vfs::Fetcher> fetcher_;
    std::unique_ptr<vfs::Filesystem> fs_;

    std::shared_ptr<transport::Endpoint> endpoint_;
    std::unique_ptr<discovery::LanDiscovery> lan_;
    std::map<core::PublicKey, std::shared_ptr<transport::Connection>> live_;
    // Dials in flight, per member and address: a stale address timing out must not hold up one
    // just found on the LAN.
    std::set<std::pair<core::PublicKey, asio::ip::udp::endpoint>> dialing_;
    std::vector<PendingInvite> invites_;
    std::map<core::PublicKey, int> member_connections_;
    std::size_t pairing_connections_ = 0;
    bool stopped_ = false;
    // Set when the Service goes away; coroutines check it after every suspension.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// Pairing, new-device side: connects to the inviter (pinning the key in the code), proves the
// token, and returns the drive's identity. Also returns the inviter address that answered.
struct JoinResult {
    DriveInfo info;
    asio::ip::udp::endpoint inviter_address;
};
asio::awaitable<core::Result<JoinResult>> request_join(transport::Endpoint& endpoint,
                                                       const Invite& invite, const Join& join);

}  // namespace ghostdrive::daemon
