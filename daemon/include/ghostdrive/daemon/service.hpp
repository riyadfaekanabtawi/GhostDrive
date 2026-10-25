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
#include "ghostdrive/transport/frame.hpp"
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
    // Advertise this device and find the others on the LAN (mDNS), when built with DNS-SD or
    // avahi-client.
    bool lan_discovery = true;
    // Rendezvous: how often to ask the server for members that are offline or only relayed,
    // and how long hole punching gets before the relay is tried.
    std::chrono::milliseconds rendezvous_interval{10'000};
    std::chrono::milliseconds relay_after{3'000};
    // Testing: skip direct attempts and go straight to the relay.
    bool relay_only = false;
    // How long a device must have been a member before this device vouches for it too, and
    // how long a removal must have stood before this device makes it permanent. A removed
    // device that adds a key, or removes another device, gets neither from a device that
    // learns of its removal within this time.
    std::chrono::milliseconds countersign_after{60'000};
    // How long records that may become valid (their author or adopter is not established here
    // yet) hold a peer's index progress back. After that they are skipped like invalid ones.
    std::chrono::milliseconds pending_hold{10 * 60'000};
    // How long a read of a chunk with no connected holder waits for one (a record naming one
    // arrives, or a holder connects) before it fails.
    std::chrono::milliseconds holder_wait{5'000};
};

struct PeerStatus {
    core::PublicKey key{};
    std::string name;
    std::string address;
    bool connected = false;
    bool relayed = false;  // connected through the rendezvous server's relay
};

struct ServiceStatus {
    std::string device_name;
    std::string fingerprint;
    std::uint16_t port = 0;
    std::string rendezvous;  // configured server, empty if none
    // Hex admission IDs for the server's --allow-group: the current discovery key's, and the
    // earlier ones a Register also lists (the last is the drive secret's, generation 0).
    std::string admission;
    std::uint64_t admission_generation = 0;
    std::vector<std::string> earlier_admissions;
    // Every member has sent a key at or above the current one since the daemon started, so
    // the earlier IDs can be dropped from allow lists.
    bool members_caught_up = false;
    std::optional<asio::ip::udp::endpoint> observed;  // public address the server sees
    std::string removed_by;  // set when a member has removed this device from the drive
    // Set when a new discovery key could not be made or saved (removed devices may still find
    // this one).
    std::string discovery_error;
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
    // Takes a member out of the drive for good, by name or fingerprint. Returns the text for
    // `ghostdrive remove`.
    core::Result<std::string> remove_member(const std::string& name_or_id);
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
    asio::awaitable<void> push_membership(std::shared_ptr<transport::Connection> conn);
    asio::awaitable<void> receive_membership(std::shared_ptr<transport::Connection> conn,
                                             std::shared_ptr<transport::Stream> stream,
                                             transport::wire::Frame first);
    // Countersigns members that are due, then, if anything changed, saves, re-pins, and
    // closes the connections of devices that are no longer members.
    core::Status refresh_membership(bool changed);
    asio::awaitable<void> push_discovery_keys(std::shared_ptr<transport::Connection> conn);
    asio::awaitable<void> receive_discovery_keys(std::shared_ptr<transport::Connection> conn,
                                                 std::shared_ptr<transport::Stream> stream,
                                                 transport::wire::Frame first);
    // Re-registers with the rendezvous server and re-advertises on the LAN under the new key.
    void on_discovery_key_changed();
    // Tells a device just removed why it is being disconnected, then closes the connection.
    asio::awaitable<void> farewell(std::shared_ptr<transport::Connection> conn);
    void on_lan_sighting(const discovery::LanSighting& sighting);
    asio::awaitable<void> rendezvous_loop();
    // A Register for the current discovery key, noting it in registered_key_.
    core::Result<core::Bytes> rendezvous_register();
    asio::awaitable<void> request_peers(std::shared_ptr<transport::Connection> conn,
                                        std::shared_ptr<transport::Stream> control);
    void on_peer_at(const core::PublicKey& peer, const asio::ip::udp::endpoint& address);
    asio::awaitable<void> punch(asio::ip::udp::endpoint address);
    asio::awaitable<void> relay_fallback(core::PublicKey peer);
    bool is_relayed(const std::shared_ptr<transport::Connection>& conn) const;
    bool dials_first(const core::PublicKey& peer) const;
    void start_dial(const core::PublicKey& peer, const asio::ip::udp::endpoint& address);

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
    std::shared_ptr<transport::Connection> rendezvous_conn_;
    // When each peer's PeerAt was last acted on: a server, honest or not, cannot make us punch
    // and dial faster than this.
    std::map<core::PublicKey, std::chrono::steady_clock::time_point> last_peer_at_;
    std::optional<asio::ip::udp::endpoint> observed_;
    std::pair<std::uint64_t, core::PublicKey> registered_key_{};
    // The highest discovery key generation each member has sent us.
    std::map<core::PublicKey, std::uint64_t> seen_generation_;
    // The same by (generation, author), for whether every member holds the current key.
    std::map<core::PublicKey, DiscoveryKeyId> seen_key_;
    // Register in the form servers from before admission lists take; flips after a refusal.
    bool fixed_register_ = false;
    // Dials prompted by LAN sightings under older keys, which a removed device can forge.
    std::map<core::PublicKey, std::chrono::steady_clock::time_point> last_old_key_dial_;
    std::chrono::steady_clock::time_point old_key_window_{};
    int old_key_dials_ = 0;
    std::string discovery_error_;
    std::map<core::PublicKey, std::shared_ptr<transport::Connection>> live_;
    // Dials in flight, per member and address: a stale address timing out must not hold up one
    // just found on the LAN.
    std::set<std::pair<core::PublicKey, asio::ip::udp::endpoint>> dialing_;
    std::vector<PendingInvite> invites_;
    std::map<core::PublicKey, int> member_connections_;
    // Every open member connection, including the second one when both sides dialed.
    std::set<std::shared_ptr<transport::Connection>> member_conns_;
    // Connections that sent records that may become valid, with the devices they name. A
    // connection is closed when one of those devices is pinned or retired, so the next one
    // sends the records again.
    std::map<std::shared_ptr<transport::Connection>, std::set<std::uint64_t>> pending_conns_;
    // Counts pins and retirements, and when each device last changed, so a batch applied
    // meanwhile notices one.
    std::uint64_t signer_epoch_ = 0;
    std::map<std::uint64_t, std::uint64_t> changed_at_;
    // Since when each peer's progress has been held back by such records (pending_hold).
    std::map<core::PublicKey, std::chrono::steady_clock::time_point> pending_since_;
    // When each member was first seen as one, for countersign_after.
    std::map<core::PublicKey, std::chrono::steady_clock::time_point> member_since_;
    // When each device was first seen removed by a member, for the same delay.
    std::map<core::PublicKey, std::chrono::steady_clock::time_point> removed_since_;
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
