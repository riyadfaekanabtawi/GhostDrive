#include "ghostdrive/daemon/service.hpp"

#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif
#include <openssl/crypto.h>

#include <algorithm>
#include <charconv>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#ifdef _WIN32
// After Asio, which includes winsock2.h, a prerequisite of iphlpapi.h.
#include <iphlpapi.h>
#endif

#include "async.hpp"
#include "ghostdrive/discovery/lan_discovery.hpp"
#include "ghostdrive/discovery/lan_id.hpp"
#include "ghostdrive/discovery/rendezvous_codec.hpp"
#include "ghostdrive/transport/demux.hpp"
#include "ghostdrive/transport/frame.hpp"
#include "ghostdrive/vfs/chunk_exchange.hpp"

namespace ghostdrive::daemon {

using core::Errc;
using core::Error;
using core::Status;
namespace wire = transport::wire;
using Udp = asio::ip::udp;

namespace {

constexpr std::size_t kBatchRecords = 256;
// Devices remembered per connection whose pin or retirement closes it (pending records).
constexpr std::size_t kMaxPendingIds = 64;
// The first frame of any stream is small: a GetChunk, Hello or Join.
constexpr std::uint32_t kMaxFirstFrame = 4096;
constexpr std::uint32_t kMaxDriveInfo = 8 * 1024 * 1024;  // the largest membership log fits
constexpr std::uint32_t kMaxMembershipFrame = 256 * 1024;
constexpr std::uint32_t kMaxDiscoveryKeysFrame = 16 * 1024;
constexpr std::chrono::seconds kPairingTimeout{15};
constexpr std::size_t kMaxPairingConnections = 8;
constexpr int kMaxConnectionsPerMember = 2;

asio::awaitable<void> sleep_for(std::chrono::milliseconds duration) {
    asio::steady_timer timer(co_await asio::this_coro::executor, duration);
    std::error_code ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

std::optional<Udp::endpoint> parse_address(const std::string& text) {
    auto colon = text.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    std::error_code ec;
    auto host = text.substr(0, colon);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    auto address = asio::ip::make_address(host, ec);
    if (ec) return std::nullopt;
    unsigned long port = 0;
    auto digits = std::string_view(text).substr(colon + 1);
    auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (err != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
    if (port == 0 || port > 65535) return std::nullopt;
    return Udp::endpoint(address, static_cast<std::uint16_t>(port));
}

std::string format_address(const Udp::endpoint& ep) {
    auto host = ep.address().to_string();
    if (ep.address().is_v6()) host = "[" + host + "]";
    return host + ":" + std::to_string(ep.port());
}

// Non-loopback IPv4 addresses of interfaces that are up.
std::vector<asio::ip::address> local_ipv4_addresses() {
    std::vector<asio::ip::address> out;
#ifdef _WIN32
    // Microsoft recommends starting with a 15 KB buffer and growing it when told to.
    std::vector<std::byte> buffer(15 * 1024);
    ULONG size = static_cast<ULONG>(buffer.size());
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = ::GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* a = adapter->FirstUnicastAddress; a; a = a->Next) {
            const sockaddr* sa = a->Address.lpSockaddr;
            if (sa == nullptr || sa->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
            asio::ip::address_v4 v4(ntohl(sin->sin_addr.s_addr));
            if (v4.is_loopback()) continue;
            if (std::find(out.begin(), out.end(), asio::ip::address(v4)) == out.end())
                out.push_back(v4);
        }
    }
    return out;
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (auto* it = list; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) continue;
        if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) continue;
        auto* sin = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        asio::ip::address_v4 v4(ntohl(sin->sin_addr.s_addr));
        if (std::find(out.begin(), out.end(), asio::ip::address(v4)) == out.end())
            out.push_back(v4);
    }
    ::freeifaddrs(list);
    return out;
#endif
}

std::set<core::PublicKey> member_keys(const MembershipView& view) {
    std::set<core::PublicKey> out;
    for (const auto& [key, name] : view.members) out.insert(key);
    return out;
}

struct AdmissionId {
    std::uint64_t generation = 0;
    discovery::GroupId id{};
};

// The admission IDs a Register lists: the current key's, then the earlier keys', then the
// drive secret's (generation 0), which is what allow lists held before IDs rotated.
core::Result<std::vector<AdmissionId>> admission_ids(const DiscoveryKeys& keys) {
    std::vector<AdmissionId> out;
    auto add = [&](std::uint64_t generation, const core::DriveSecret& key) -> core::Status {
        auto id = generation == 0 ? discovery::rendezvous_group(key)
                                  : discovery::rendezvous_admission(key);
        if (!id) return id.error();
        out.push_back({generation, id.value()});
        return {};
    };
    if (auto st = add(keys.current_id().first, keys.current()); !st) return st.error();
    for (const auto& held : keys.earlier()) {
        // The generation-0 ID always fits: kMaxAdmissions is the current key, the earlier ones
        // and the drive secret.
        if (held.generation != 0 && out.size() + 1 >= discovery::kMaxAdmissions) continue;
        if (auto st = add(held.generation, *held.key); !st) return st.error();
    }
    return out;
}

}  // namespace

Service::Service(asio::any_io_executor io, asio::any_io_executor disk, Drive& drive,
                 ServiceOptions options)
    : io_(std::move(io)),
      disk_(std::move(disk)),
      drive_(drive),
      options_(std::move(options)),
      cache_(options_.cache_bytes) {
    fetcher_ = std::make_unique<vfs::Fetcher>(
        io_, disk_, drive_.store(), drive_.index(), cache_,
        [this](const core::PublicKey& key) -> std::shared_ptr<transport::Connection> {
            auto it = live_.find(key);
            return it == live_.end() ? nullptr : it->second;
        },
        std::chrono::seconds(10), options_.holder_wait);
    fs_ = std::make_unique<vfs::Filesystem>(
        io_, tree_, *fetcher_, vfs::FilesystemOptions{},
        vfs::WriteSupport{drive_.store(), drive_.index(), drive_.clock(), disk_,
                          drive_.paths().staging});
}

Service::~Service() {
    *alive_ = false;
    if (endpoint_) endpoint_->close();
}

Status Service::start() {
    if (auto refreshed = fs_->refresh(drive_.index()); !refreshed) return refreshed.error();
    if (auto st = refresh_membership(true); !st) return st;

    transport::EndpointOptions options;
    options.bind = options_.listen;
    options.impairment = options_.impairment;
    // Members connect by pinned key. While an invite is open, anyone may complete a handshake,
    // but such a connection only ever reaches the pairing exchange (see run_pairing).
    options.accept_peer = [this](const core::PublicKey& key) {
        return is_member(key) || invites_active();
    };
    // Keeps NAT mappings open for member and rendezvous connections.
    options.keep_alive = std::chrono::seconds(15);
    auto endpoint = transport::Endpoint::create(io_, drive_.key(), options);
    if (!endpoint) return endpoint.error();
    endpoint_ = std::move(endpoint).value();

    asio::co_spawn(io_, accept_loop(), asio::detached);
    asio::co_spawn(io_, dial_loop(), asio::detached);
    asio::co_spawn(io_, rendezvous_loop(), asio::detached);
#ifdef GHOSTDRIVE_HAVE_LAN_DISCOVERY
    if (options_.lan_discovery) {
        auto lan = discovery::LanDiscovery::start(
            io_, port(),
            [this] {
                return discovery::lan_id(drive_.discovery_keys().current(),
                                         drive_.key().public_key(),
                                         discovery::lan_epoch(std::chrono::system_clock::now()));
            },
            [this](const discovery::LanSighting& s) { on_lan_sighting(s); });
        // Without mDNS the drive still works through stored addresses.
        if (lan) lan_ = std::move(lan).value();
    }
#endif
    return {};
}

void Service::on_lan_sighting(const discovery::LanSighting& sighting) {
    if (stopped_) return;
    auto devices = drive_.index().devices();
    if (!devices) return;
    std::vector<core::PublicKey> members;
    for (const auto& d : devices.value()) members.push_back(d.key);
    std::optional<core::PublicKey> key;
    std::uint64_t generation = 0;
    bool current = false;
    auto now = std::chrono::system_clock::now();
    auto held = drive_.discovery_keys().all();
    for (std::size_t i = 0; i < held.size() && !key; ++i) {
        key = discovery::match_lan_id(sighting.id, *held[i].key, members, now);
        generation = held[i].generation;
        current = i == 0;
    }
    if (!key) return;  // another drive, or a stale or forged advertisement
    if (!current) {
        // An older key: a member that has not caught up, or a removed device posing as one (it
        // knows every key from before its removal). Only dial, which pins the member's key, and
        // keep the stored address: a forgery must not replace it. A member seen holding a newer
        // key no longer advertises this one.
        if (auto it = seen_generation_.find(*key);
            it != seen_generation_.end() && it->second > generation) {
            return;
        }
        // Forged sightings must not take the dial slots real ones need: once a minute per
        // member, and a few per window in all.
        auto at = std::chrono::steady_clock::now();
        if (at - old_key_window_ > std::chrono::seconds(10)) {
            old_key_window_ = at;
            old_key_dials_ = 0;
        }
        auto& last = last_old_key_dial_[*key];
        if (live_.contains(*key) || at - last < std::chrono::minutes(1) || old_key_dials_ >= 4) {
            return;
        }
        last = at;
        ++old_key_dials_;
        start_dial(*key, sighting.endpoint);
        return;
    }
    auto text = format_address(sighting.endpoint);
    auto known = drive_.peer_addresses();
    if (auto it = known.find(*key); it == known.end() || it->second != text) {
        (void)drive_.set_peer_address(*key, text);
    }
    if (auto it = live_.find(*key); it != live_.end() && !is_relayed(it->second)) return;
    start_dial(*key, sighting.endpoint);
}

void Service::start_dial(const core::PublicKey& peer, const Udp::endpoint& address) {
    // Bounded: hints from mDNS or the server may name many addresses.
    constexpr std::size_t kMaxDials = 32;
    if (stopped_ || dialing_.size() >= kMaxDials || dialing_.contains({peer, address})) return;
    dialing_.insert({peer, address});
    asio::co_spawn(io_, dial(peer, address), asio::detached);
}

bool Service::is_relayed(const std::shared_ptr<transport::Connection>& conn) const {
    return endpoint_ && endpoint_->is_relay_address(conn->current_remote_endpoint());
}

// QUIC needs one client and one server; when both learn of each other at once, the device with
// the smaller key dials (ARCHITECTURE.md section 2.6) and the other waits to accept.
bool Service::dials_first(const core::PublicKey& peer) const {
    return drive_.key().public_key() < peer;
}

asio::awaitable<void> Service::rendezvous_loop() {
    auto alive = alive_;
    std::chrono::milliseconds backoff{1'000};
    while (*alive && !stopped_) {
        auto config = drive_.rendezvous();
        if (!config) {
            co_await sleep_for(std::chrono::seconds(5));
            continue;
        }
        Udp::resolver resolver(io_);
        std::error_code ec;
        auto results = co_await resolver.async_resolve(
            Udp::v4(), config->host, std::to_string(config->port),
            asio::redirect_error(asio::use_awaitable, ec));
        if (!*alive || stopped_) co_return;
        std::shared_ptr<transport::Connection> conn;
        Udp::endpoint server;
        if (!ec && !results.empty()) {
            server = results.begin()->endpoint();
            // Datagrams: relayed packets travel as DATAGRAM frames on this connection.
            auto connected = co_await endpoint_->connect(server, config->key, {.datagrams = true});
            if (!*alive || stopped_) co_return;
            if (connected) conn = std::move(connected).value();
        }
        if (!conn) {
            co_await sleep_for(backoff);
            backoff = std::min<std::chrono::milliseconds>(backoff * 2, std::chrono::seconds(30));
            continue;
        }

        auto control = co_await conn->open_stream();
        bool registered = false;
        bool sent = false;
        auto request = rendezvous_register();
        if (control && request) {
            if (co_await control.value()->write(request.value())) {
                sent = true;
                auto reply = co_await wire::read_frame(*control.value(), 64);
                if (reply && reply.value().type == wire::FrameType::rendezvous_registered) {
                    if (auto r = discovery::decode_registered(reply.value().payload)) {
                        observed_ = r.value().observed;
                        // Relayed packets are accepted only on this connection and only from
                        // members; QUIC then authenticates the member itself.
                        endpoint_->set_relay(
                            conn, [this](const core::PublicKey& key) { return is_member(key); });
                        registered = true;
                    }
                }
            }
        }
        if (!*alive) co_return;
        // A server from before admission lists closes on the newer Register, and one with an
        // allow list may admit only one of the two forms: the next attempt uses the other.
        if (sent && !registered) fixed_register_ = !fixed_register_;
        if (registered) {
            backoff = std::chrono::milliseconds(1'000);
            rendezvous_conn_ = conn;
            asio::co_spawn(io_, request_peers(conn, control.value()), asio::detached);
            for (;;) {
                auto frame = co_await wire::read_frame(*control.value(), 64);
                if (!*alive) co_return;
                if (frame && frame.value().type == wire::FrameType::rendezvous_registered) {
                    // The answer to registering again under a new discovery key.
                    if (auto r = discovery::decode_registered(frame.value().payload)) {
                        observed_ = r.value().observed;
                    }
                    continue;
                }
                if (!frame || frame.value().type != wire::FrameType::rendezvous_peer_at) break;
                auto at = discovery::decode_peer_at(frame.value().payload);
                if (!at) break;
                if (at.value().address) on_peer_at(at.value().peer, *at.value().address);
            }
        }
        conn->close();
        if (rendezvous_conn_ == conn) rendezvous_conn_.reset();
        observed_.reset();
        endpoint_->clear_relay();
        co_await sleep_for(backoff);
    }
}

core::Result<core::Bytes> Service::rendezvous_register() {
    // The group follows the discovery key, so a removed device cannot meet members there; the
    // server admits the drive by any of its admission IDs (see admission_ids). Members that
    // missed a change of key are still in the groups of earlier keys, so this device looks for
    // them there too, without being found there itself.
    auto& keys = drive_.discovery_keys();
    auto group = discovery::rendezvous_group(keys.current());
    if (!group) return group.error();
    auto admissions = admission_ids(keys);
    if (!admissions) return admissions.error();
    discovery::Register reg{group.value(), {}, {}};
    for (const auto& a : admissions.value()) reg.admissions.push_back(a.id);
    for (const auto& earlier : keys.earlier()) {
        if (reg.hidden.size() == discovery::kMaxHiddenGroups) break;
        if (auto g = discovery::rendezvous_group(*earlier.key)) reg.hidden.push_back(g.value());
    }
    registered_key_ = keys.current_id();
    return fixed_register_ ? discovery::encode_fixed_register(reg) : discovery::encode(reg);
}

asio::awaitable<void> Service::request_peers(std::shared_ptr<transport::Connection> conn,
                                             std::shared_ptr<transport::Stream> control) {
    auto alive = alive_;
    // The only writer on the control stream once registered (one write at a time).
    auto next_round = std::chrono::steady_clock::now();
    while (*alive && !stopped_ && conn->is_open()) {
        // A new discovery key moves the registration without dropping the connection, so the
        // relay, and members reached only through it, stay up while the key spreads.
        if (drive_.discovery_keys().current_id() != registered_key_) {
            auto request = rendezvous_register();
            if (!request || !co_await control->write(request.value())) co_return;
            if (!*alive) co_return;
        }
        if (std::chrono::steady_clock::now() < next_round) {
            co_await sleep_for(std::min(options_.sync_poll, options_.rendezvous_interval));
            continue;
        }
        next_round = std::chrono::steady_clock::now() + options_.rendezvous_interval;
        if (auto devices = drive_.index().devices()) {
            for (const auto& d : devices.value()) {
                // Members not reachable directly: offline here, or only through the relay.
                auto it = live_.find(d.key);
                if (it != live_.end() && !is_relayed(it->second)) continue;
                auto request = discovery::encode(discovery::Connect{d.key});
                if (!co_await control->write(request)) co_return;
                if (!*alive) co_return;
            }
        }
    }
}

void Service::on_peer_at(const core::PublicKey& peer, const Udp::endpoint& address) {
    if (stopped_) return;
    auto now = std::chrono::steady_clock::now();
    auto& last = last_peer_at_[peer];
    if (now - last < std::chrono::seconds(2)) return;
    last = now;
    if (!is_member(peer)) {
        last_peer_at_.erase(peer);
        return;
    }
    if (auto it = live_.find(peer); it != live_.end() && !is_relayed(it->second)) return;
    if (!options_.relay_only) {
        // Both sides probe each other's public address, opening their NAT mappings, and the
        // first device dials once the other's probes are on their way.
        asio::co_spawn(io_, punch(address), asio::detached);
        if (dials_first(peer)) start_dial(peer, address);
    }
    if (!live_.contains(peer)) asio::co_spawn(io_, relay_fallback(peer), asio::detached);
}

asio::awaitable<void> Service::punch(Udp::endpoint address) {
    auto alive = alive_;
    // Probes carry nothing, not even our key: they only open the NAT mapping.
    auto probe = transport::make_probe();
    for (int i = 0; i < 10 && *alive && !stopped_; ++i) {
        endpoint_->send_datagram(probe, address);
        co_await sleep_for(std::chrono::milliseconds(200));
    }
}

asio::awaitable<void> Service::relay_fallback(core::PublicKey peer) {
    auto alive = alive_;
    if (!options_.relay_only) co_await sleep_for(options_.relay_after);
    if (!*alive || stopped_ || live_.contains(peer) || !dials_first(peer)) co_return;
    if (auto relay = endpoint_->relay_address(peer)) start_dial(peer, *relay);
}

void Service::stop() {
    stopped_ = true;
    lan_.reset();
    // Members first, while the relay can still carry their close.
    for (auto& [key, conn] : live_) conn->close();
    live_.clear();
    if (rendezvous_conn_) rendezvous_conn_->close();
    if (endpoint_) endpoint_->close();
}

std::uint16_t Service::port() const {
    return endpoint_ ? endpoint_->local_endpoint().port() : 0;
}

bool Service::is_member(const core::PublicKey& key) const {
    auto pinned = drive_.index().is_pinned(key);
    return pinned && pinned.value();
}

bool Service::invites_active() {
    auto now = std::chrono::steady_clock::now();
    std::erase_if(invites_, [now](const auto& i) { return i.expires <= now; });
    return !invites_.empty();
}

core::Result<std::string> Service::create_invite(std::chrono::seconds ttl) {
    if (!endpoint_) return Error{Errc::closed, "service is not running"};
    PendingInvite pending{};
    if (auto st = core::random_bytes(pending.token); !st) return st.error();
    pending.expires = std::chrono::steady_clock::now() + ttl;
    invites_.push_back(pending);

    Invite invite;
    invite.inviter = drive_.key().public_key();
    invite.token = pending.token;
    auto addresses = options_.invite_addresses;
    if (addresses.empty()) {
        addresses = local_ipv4_addresses();
        addresses.push_back(asio::ip::address_v4::loopback());
    }
    for (const auto& a : addresses) invite.addresses.emplace_back(a, port());
    return encode_invite(invite);
}

core::Result<std::string> Service::remove_member(const std::string& name_or_id) {
    // Any device known here can be removed, including one in a contest nobody settled.
    auto view = drive_.membership().view();
    std::optional<core::PublicKey> target;
    if (auto id = core::parse_fingerprint(name_or_id); id && view.names.contains(*id)) {
        target = *id;
    } else {
        for (const auto& [key, name] : view.names) {
            if (name != name_or_id || view.removed.contains(key)) continue;
            if (target) {
                return Error{Errc::invalid_argument,
                             "more than one device is called " + name_or_id +
                                 "; give its ID from `ghostdrive status` instead"};
            }
            target = key;
        }
    }
    if (!target) {
        if (name_or_id == drive_.device_name() ||
            core::parse_fingerprint(name_or_id) == drive_.key().public_key()) {
            return Error{Errc::invalid_argument,
                         "this device cannot remove itself; remove it from another device"};
        }
        return Error{Errc::not_found, "no device called " + name_or_id + " in this drive"};
    }
    if (auto st = drive_.membership().remove(drive_.key(), *target); !st) return st.error();
    if (auto st = refresh_membership(true); !st) return st.error();

    std::string out = "Removed " + view.names[*target] + " (" + core::fingerprint(*target) +
                      "). It can no longer connect to this drive's devices.\n";
    // Devices it vouched for that no other device invited: it may have made them up.
    for (const auto& [key, name] : view.members) {
        if (key == *target || !view.vouched_by[key].contains(*target)) continue;
        auto& inviters = view.invited_by[key];
        if (std::any_of(inviters.begin(), inviters.end(),
                        [&](const core::PublicKey& k) { return k != *target; })) {
            continue;
        }
        out += name + " (" + core::fingerprint(key) + ") was added by it. If you do not " +
               "recognise it, remove it too: ghostdrive remove " + core::fingerprint(key) + "\n";
    }
    return out;
}

ServiceStatus Service::status() const {
    ServiceStatus out;
    out.device_name = drive_.device_name();
    out.fingerprint = core::fingerprint(drive_.key().public_key());
    out.port = port();
    if (auto config = drive_.rendezvous()) out.rendezvous = config->host + ":" + std::to_string(config->port);
    out.observed = observed_;
    out.discovery_error = discovery_error_;
    auto view = drive_.membership().view();
    if (view.removed_by) {
        auto it = view.members.find(*view.removed_by);
        out.removed_by = it != view.members.end() ? it->second : core::fingerprint(*view.removed_by);
    }
    auto& keys = drive_.discovery_keys();
    if (auto ids = admission_ids(keys)) {
        out.admission = core::to_hex(ids.value().front().id);
        out.admission_generation = ids.value().front().generation;
        for (std::size_t i = 1; i < ids.value().size(); ++i) {
            out.earlier_admissions.push_back(core::to_hex(ids.value()[i].id));
        }
    }
    out.members_caught_up = members_hold(seen_key_, member_keys(view),
                                         drive_.key().public_key(), keys.current_id());
    auto addresses = drive_.peer_addresses();
    if (auto devices = drive_.index().devices()) {
        for (const auto& d : devices.value()) {
            PeerStatus p;
            p.key = d.key;
            p.name = d.name;
            if (auto it = addresses.find(d.key); it != addresses.end()) p.address = it->second;
            if (auto it = live_.find(d.key); it != live_.end()) {
                p.connected = true;
                p.relayed = is_relayed(it->second);
            }
            out.peers.push_back(std::move(p));
        }
    }
    return out;
}

asio::awaitable<void> Service::accept_loop() {
    auto alive = alive_;
    auto endpoint = endpoint_;
    for (;;) {
        auto conn = co_await endpoint->accept();
        if (!*alive || stopped_ || !conn) co_return;
        if (is_member(conn.value()->peer_key())) {
            asio::co_spawn(io_, run_member(std::move(conn).value()), asio::detached);
        } else if (pairing_connections_ >= kMaxPairingConnections) {
            // While an invite is open anyone can complete a handshake; bound what that costs.
            conn.value()->close();
        } else {
            asio::co_spawn(io_, run_pairing(std::move(conn).value()), asio::detached);
        }
    }
}

asio::awaitable<void> Service::dial_loop() {
    auto alive = alive_;
    while (*alive && !stopped_) {
        (void)refresh_membership(false);  // vouches for members once they are due
        for (const auto& [key, text] : drive_.peer_addresses()) {
            if (live_.contains(key) || !is_member(key)) continue;
            auto address = parse_address(text);
            if (!address) continue;
            start_dial(key, *address);
        }
        co_await sleep_for(options_.redial);
    }
}

asio::awaitable<void> Service::dial(core::PublicKey key, Udp::endpoint address) {
    auto alive = alive_;
    auto endpoint = endpoint_;
    auto conn = co_await endpoint->connect(address, key);
    if (!*alive) co_return;
    dialing_.erase({key, address});
    if (!conn || stopped_) co_return;
    // Both sides may dial at once; the later connection simply replaces the earlier one.
    co_await run_member(std::move(conn).value());
}

asio::awaitable<void> Service::run_member(std::shared_ptr<transport::Connection> conn) {
    auto alive = alive_;
    auto key = conn->peer_key();
    // Both sides may dial at once, so two connections per member are normal; more is a peer
    // multiplying its resource use. A dial started before a removal can complete after it.
    if (!is_member(key) || member_connections_[key] >= kMaxConnectionsPerMember) {
        conn->close();
        co_return;
    }
    ++member_connections_[key];
    member_conns_.insert(conn);
    // Prefer a direct path: a relayed connection never replaces a live direct one, and a new
    // direct one retires the relayed one it upgrades.
    auto previous = live_.find(key);
    bool keep_previous = previous != live_.end() && previous->second->is_open() &&
                         !is_relayed(previous->second) && is_relayed(conn);
    if (!keep_previous) {
        if (previous != live_.end() && previous->second != conn && is_relayed(previous->second) &&
            !is_relayed(conn)) {
            previous->second->close();
        }
        live_[key] = conn;
        fetcher_->holders_changed();
    }
    asio::co_spawn(io_, push_index(conn), asio::detached);
    asio::co_spawn(io_, push_membership(conn), asio::detached);
    asio::co_spawn(io_, push_discovery_keys(conn), asio::detached);
    for (;;) {
        auto stream = co_await conn->accept_stream();
        if (!*alive) co_return;
        if (!stream) break;  // connection closed
        // A device removed while connected gets nothing more.
        if (!is_member(key)) continue;
        asio::co_spawn(io_, handle_stream(conn, std::move(stream).value()), asio::detached);
    }
    member_conns_.erase(conn);
    pending_conns_.erase(conn);
    if (auto it = live_.find(key); it != live_.end() && it->second == conn) live_.erase(it);
    if (--member_connections_[key] == 0) member_connections_.erase(key);
}

asio::awaitable<void> Service::handle_stream(std::shared_ptr<transport::Connection> conn,
                                             std::shared_ptr<transport::Stream> stream) {
    auto alive = alive_;
    auto first = co_await wire::read_frame(*stream, kMaxFirstFrame);
    if (!*alive || !first || !is_member(conn->peer_key())) co_return;
    switch (first.value().type) {
        case wire::FrameType::get_chunk:
            co_await vfs::answer_chunk_request(stream, first.value(), drive_.store(), disk_);
            break;
        case wire::FrameType::hello: {
            auto hello = decode_hello(first.value().payload);
            if (!hello) {
                conn->close();
                co_return;
            }
            co_await receive_index(conn, stream, hello.value());
            break;
        }
        case wire::FrameType::membership:
            co_await receive_membership(conn, stream, std::move(first).value());
            break;
        case wire::FrameType::discovery_keys:
            co_await receive_discovery_keys(conn, stream, std::move(first).value());
            break;
        default: {
            auto reply = wire::encode_error(wire::ErrorCode::bad_request);
            (void)co_await stream->write(reply, true);
            break;
        }
    }
}

asio::awaitable<void> Service::push_index(std::shared_ptr<transport::Connection> conn) {
    auto alive = alive_;
    auto* drive = &drive_;
    auto stream = co_await conn->open_stream();
    if (!*alive || !stream) co_return;
    auto max = co_await detail::run_on(disk_, [drive] { return drive->index().max_sequence(); });
    if (!*alive || !max) co_return;
    auto hello = encode_hello(Hello{drive_.index().index_id(), max.value()});
    if (!co_await stream.value()->write(hello)) co_return;

    auto request_frame = co_await wire::read_frame(*stream.value(), 64);
    if (!*alive || !request_frame || request_frame.value().type != wire::FrameType::index_request) {
        co_return;
    }
    auto request = decode_index_request(request_frame.value().payload);
    if (!request) co_return;
    std::uint64_t after = request.value().after;

    while (*alive && conn->is_open() && is_member(conn->peer_key())) {
        auto batch = co_await detail::run_on(
            disk_, [drive, after]() -> core::Result<std::vector<IndexEntry>> {
                auto records = drive->index().records_since(after, kBatchRecords, true);
                if (!records) return records.error();
                std::vector<IndexEntry> entries;
                entries.reserve(records.value().size());
                for (auto& record : records.value()) {
                    bool holds =
                        !record.deleted && std::all_of(record.chunks.begin(), record.chunks.end(),
                                                       [drive](const auto& id) {
                                                           return drive->store().contains(id);
                                                       });
                    entries.push_back(IndexEntry{std::move(record), holds});
                }
                return entries;
            });
        if (!*alive || !batch || !is_member(conn->peer_key())) co_return;
        if (batch.value().empty()) {
            co_await sleep_for(options_.sync_poll);
            continue;
        }
        after = batch.value().back().record.sequence;
        auto frame = encode_index_batch(batch.value());
        if (!co_await stream.value()->write(frame)) co_return;
    }
}

asio::awaitable<void> Service::receive_index(std::shared_ptr<transport::Connection> conn,
                                             std::shared_ptr<transport::Stream> stream,
                                             Hello hello) {
    auto alive = alive_;
    auto* drive = &drive_;
    auto* fs = fs_.get();
    auto peer = conn->peer_key();

    // Resume where the last exchange with this index stopped; a new index_id means the peer's
    // index was recreated, so start over.
    auto after = co_await detail::run_on(disk_, [drive, peer, hello]() -> std::uint64_t {
        auto devices = drive->index().devices();
        if (!devices) return 0;
        for (const auto& d : devices.value()) {
            if (d.key == peer && d.peer_index_id == hello.index_id) return d.peer_max_sequence;
        }
        return 0;
    });
    if (!*alive) co_return;
    auto request = encode_index_request(IndexRequest{after});
    if (!co_await stream->write(request)) co_return;

    // Progress on this stream never passes a record that may still become valid (its signer
    // is not known yet), so the next connection sends it again. held names the devices those
    // records wait for (bounded).
    std::optional<std::uint64_t> cap;
    std::set<std::uint64_t> held;
    for (;;) {
        auto frame = co_await wire::read_frame(*stream);
        if (!*alive || !frame) co_return;
        if (frame.value().type != wire::FrameType::index_batch2) {
            conn->close();
            co_return;
        }
        auto entries = decode_index_batch(frame.value().payload);
        if (!entries) {
            conn->close();
            co_return;
        }
        if (!is_member(peer)) co_return;
        // Devices whose records in this batch may become valid once pinned or retired here.
        std::set<std::uint64_t> pending;
        bool any_pending = false;
        const auto epoch = signer_epoch_;
        const auto now = std::chrono::steady_clock::now();
        // Such records hold this peer's progress back for pending_hold at most, so keys that
        // are never established here cannot make every connection resend them.
        auto since = pending_since_.find(peer);
        const bool may_hold =
            since == pending_since_.end() || now - since->second < options_.pending_hold;
        auto applied = co_await detail::run_on(
            disk_, [drive, fs, peer, hello, may_hold, &pending, &any_pending, &cap, &held,
                    entries = std::move(entries).value()]() -> Status {
                auto& index = drive->index();
                std::uint64_t last = 0;
                Status failure;
                // Once the hold is over the stream stops holding back, for earlier batches too;
                // what it held is remembered as skipped.
                if (!may_hold && cap) {
                    for (auto id : held) (void)index.note_skipped(peer, id);
                    held.clear();
                    cap.reset();
                }
                for (const auto& e : entries) {
                    // apply_remote validates the record (paths, block size, clocks) and its
                    // signature first.
                    auto outcome = index.apply_remote(e.record, drive->clock());
                    if (!outcome && outcome.error().code == Errc::not_found) {
                        any_pending = true;
                        if (may_hold && e.record.sequence > 0) {
                            pending.insert(e.record.modified_by);
                            if (e.record.countersigner != 0) pending.insert(e.record.countersigner);
                            cap = std::min(cap.value_or(UINT64_MAX), e.record.sequence - 1);
                            for (auto id : {e.record.modified_by, e.record.countersigner}) {
                                if (id != 0 && held.size() < kMaxPendingIds) held.insert(id);
                            }
                        } else {
                            // Skipped: remembered, so pinning or retiring the device fetches
                            // this peer's index again.
                            (void)index.note_skipped(peer, e.record.modified_by);
                            if (e.record.countersigner != 0) {
                                (void)index.note_skipped(peer, e.record.countersigner);
                            }
                        }
                    } else if (!outcome && outcome.error().code != Errc::invalid_argument) {
                        // A database failure: stop here so the next exchange resends this record.
                        failure = outcome.error();
                        break;
                    }
                    // Any other rejected record will never become valid; skip past it.
                    last = std::max(last, e.record.sequence);
                    if (!outcome || !e.sender_holds || e.record.deleted) continue;
                    // Only believe "I hold these chunks" for the version we now store, so a
                    // peer cannot attach claims to stale or ignored records.
                    auto stored = index.get(e.record.path);
                    if (!stored || !stored.value() || stored.value()->version != e.record.version) {
                        continue;
                    }
                    for (const auto& id : e.record.chunks) (void)index.set_holds(peer, id, true);
                }
                const std::uint64_t progress = cap ? std::min(last, *cap) : last;
                if (progress > 0) {
                    if (auto st = index.set_peer_progress(peer, hello.index_id, progress); !st) {
                        return st;
                    }
                }
                if (!failure) return failure;
                auto refreshed = fs->refresh(index);
                if (!refreshed) return refreshed.error();
                return {};
            });
        if (!*alive) co_return;
        fetcher_->holders_changed();
        if (!any_pending || pending.empty()) {
            // Nothing held back now (none pending, or the hold ran out and they were skipped):
            // the next pending record starts a new hold.
            pending_since_.erase(peer);
        } else {
            pending_since_.emplace(peer, now);
            // One of those devices pinned or retired while the batch was applied may make the
            // records valid already; otherwise wait for that.
            bool changed = std::any_of(pending.begin(), pending.end(), [&](std::uint64_t id) {
                auto it = changed_at_.find(id);
                return it != changed_at_.end() && it->second > epoch;
            });
            if (changed) {
                conn->close();
            } else if (conn->is_open()) {
                // Bounded, so a member naming random devices cannot grow it.
                auto& ids = pending_conns_[conn];
                for (auto id : pending) {
                    if (ids.size() >= kMaxPendingIds) break;
                    ids.insert(id);
                }
            }
        }
        if (!applied) {
            conn->close();  // resume from the stored progress on the next connection
            co_return;
        }
    }
}

asio::awaitable<void> Service::run_pairing(std::shared_ptr<transport::Connection> conn) {
    auto alive = alive_;
    ++pairing_connections_;
    struct Release {
        std::size_t& count;
        std::shared_ptr<bool> alive;
        ~Release() {
            if (*alive) --count;
        }
    } release{pairing_connections_, alive};
    // An unpaired peer gets one stream and a deadline; it never reaches chunks or the index.
    asio::steady_timer deadline(io_, kPairingTimeout);
    deadline.async_wait([conn](const std::error_code& ec) {
        if (!ec) conn->close();
    });

    auto stream = co_await conn->accept_stream();
    if (!*alive || !stream) co_return;
    auto frame = co_await wire::read_frame(*stream.value(), kMaxFirstFrame);
    if (!*alive || !frame || frame.value().type != wire::FrameType::join) {
        conn->close();
        co_return;
    }
    auto join = decode_join(frame.value().payload);
    bool valid = false;
    if (join) {
        auto now = std::chrono::steady_clock::now();
        for (auto it = invites_.begin(); it != invites_.end(); ++it) {
            if (it->expires > now &&
                CRYPTO_memcmp(it->token.data(), join.value().token.data(), kTokenSize) == 0) {
                core::secure_zero(it->token);
                invites_.erase(it);  // one-time
                valid = true;
                break;
            }
        }
    }
    if (!valid) {
        auto reply = wire::encode_error(wire::ErrorCode::bad_request);
        (void)co_await stream.value()->write(reply, true);
        conn->close();
        co_return;
    }

    auto peer = conn->peer_key();
    auto address =
        format_address(Udp::endpoint(conn->remote_endpoint().address(), join.value().listen_port));
    // A removed key stays removed, invite or not; the device must join with a new key.
    bool admitted = !drive_.membership().view().removed.contains(peer) &&
                    drive_.membership().add(drive_.key(), peer, join.value().name, true) &&
                    refresh_membership(true);
    if (!admitted || !is_member(peer) || !drive_.set_peer_address(peer, address)) {
        auto reply = wire::encode_error(wire::ErrorCode::bad_request);
        (void)co_await stream.value()->write(reply, true);
        conn->close();
        co_return;
    }

    DriveInfo info;
    info.drive_id = drive_.drive_id();
    info.secret = drive_.secret();
    info.members.push_back(Member{drive_.key().public_key(), drive_.device_name(), ""});
    if (auto config = drive_.rendezvous()) info.rendezvous = config->to_string();
    info.membership = drive_.membership().entries();
    info.discovery_keys = drive_.discovery_keys().keys();
    info.retired_signers = drive_.index().retired_signers();
    auto addresses = drive_.peer_addresses();
    if (auto devices = drive_.index().devices()) {
        for (const auto& d : devices.value()) {
            if (d.key == peer) continue;
            auto it = addresses.find(d.key);
            info.members.push_back(Member{d.key, d.name, it == addresses.end() ? "" : it->second});
        }
    }
    auto reply = encode_drive_info(info);
    (void)co_await stream.value()->write(reply, true);
    core::secure_zero(reply);
    core::secure_zero(info.secret);
    for (auto& k : info.discovery_keys) core::secure_zero(k.key);
    deadline.cancel();
    // The new member reconnects as a member; this connection served only the pairing.
    conn->close();
}

Status Service::refresh_membership(bool changed) {
    auto& membership = drive_.membership();
    auto now = std::chrono::steady_clock::now();
    // Members and removals each wait countersign_after, counted from when they were first seen
    // here and reset if they stop holding.
    auto view = membership.view();
    std::erase_if(member_since_, [&](const auto& item) { return !view.members.contains(item.first); });
    std::erase_if(removed_since_,
                  [&](const auto& item) { return !view.removal_pending.contains(item.first); });
    for (const auto& [key, name] : view.members) member_since_.emplace(key, now);
    for (const auto& key : view.removal_pending) removed_since_.emplace(key, now);
    auto countersigned = membership.countersign(
        drive_.key(), [&](const core::PublicKey& key, bool removal) {
            auto& since = removal ? removed_since_ : member_since_;
            auto it = since.find(key);
            return it != since.end() && now - it->second >= options_.countersign_after;
        });
    if (!changed && (!countersigned || countersigned.value() == 0)) return {};
    if (auto st = drive_.save_membership(); !st) return st;
    auto applied = drive_.apply_membership();
    if (!applied) return applied.error();
    if (!applied.value().pinned.empty() || !applied.value().retired.empty()) {
        // Close only the connections that sent records naming one of these devices.
        ++signer_epoch_;
        std::set<std::uint64_t> changed_ids;
        for (const auto& key : applied.value().pinned) changed_ids.insert(core::short_id(key));
        for (const auto& key : applied.value().retired) changed_ids.insert(core::short_id(key));
        for (auto id : changed_ids) changed_at_[id] = signer_epoch_;
        std::vector<std::shared_ptr<transport::Connection>> stale;
        for (const auto& [conn, ids] : pending_conns_) {
            if (std::any_of(ids.begin(), ids.end(),
                            [&](std::uint64_t id) { return changed_ids.contains(id); })) {
                stale.push_back(conn);
            }
        }
        // Peers whose records naming these devices were skipped after the hold send their
        // index again.
        for (auto id : changed_ids) {
            auto peers = drive_.index().take_skipped(id);
            if (!peers) continue;
            for (const auto& peer : peers.value()) {
                (void)drive_.index().set_peer_progress(peer, 0, 0);
                for (const auto& conn : member_conns_) {
                    if (conn->peer_key() == peer) stale.push_back(conn);
                }
            }
        }
        for (const auto& conn : stale) {
            pending_conns_.erase(conn);
            conn->close();
        }
    }
    for (const auto& key : applied.value().unpinned) {
        live_.erase(key);
        member_since_.erase(key);
        bool told = false;
        for (const auto& conn : member_conns_) {
            if (conn->peer_key() != key) continue;
            if (told) {
                conn->close();
            } else {
                told = true;
                asio::co_spawn(io_, farewell(conn), asio::detached);
            }
        }
    }
    // After unpinning, so a new discovery key is never sent to a device just removed.
    auto& keys = drive_.discovery_keys();
    auto before = keys.current_id();
    view = membership.view();
    auto rotated = keys.update(drive_.key(), member_keys(view), view.removed);
    // A failure here leaves the keys as they were; the membership change itself stands, and
    // `ghostdrive status` reports it.
    core::Status saved;
    if (rotated && rotated.value()) saved = drive_.save_discovery_keys();
    if (!rotated) {
        discovery_error_ = rotated.error().message;
    } else if (!saved) {
        discovery_error_ = saved.error().message;
    } else {
        discovery_error_.clear();
    }
    if (keys.current_id() != before) on_discovery_key_changed();
    return {};
}

void Service::on_discovery_key_changed() {
    // request_peers registers again with the rendezvous server.
#ifdef GHOSTDRIVE_HAVE_LAN_DISCOVERY
    if (lan_) lan_->refresh();
#endif
}

asio::awaitable<void> Service::push_discovery_keys(std::shared_ptr<transport::Connection> conn) {
    auto alive = alive_;
    auto stream = co_await conn->open_stream();
    if (!*alive || !stream) co_return;
    // Empty first, like the membership stream, to fit the first-frame limit.
    if (!co_await stream.value()->write(encode_discovery_keys({}))) co_return;
    std::uint64_t sent = 0;
    while (*alive && conn->is_open()) {
        auto& keys = drive_.discovery_keys();
        // Checked right before every send: a device removed meanwhile gets no newer key.
        if (!is_member(conn->peer_key())) co_return;
        if (keys.version() == sent) {
            co_await sleep_for(options_.sync_poll);
            continue;
        }
        sent = keys.version();
        auto held = keys.keys();
        auto frame = encode_discovery_keys(held);
        for (auto& k : held) core::secure_zero(k.key);
        auto st = co_await stream.value()->write(frame);
        core::secure_zero(frame);
        if (!*alive || !st) co_return;
    }
}

asio::awaitable<void> Service::receive_discovery_keys(std::shared_ptr<transport::Connection> conn,
                                                      std::shared_ptr<transport::Stream> stream,
                                                      wire::Frame first) {
    auto alive = alive_;
    auto frame = core::Result<wire::Frame>(std::move(first));
    for (;;) {
        if (frame.value().type != wire::FrameType::discovery_keys) {
            conn->close();
            co_return;
        }
        auto keys = decode_discovery_keys(frame.value().payload);
        core::secure_zero(frame.value().payload);
        if (!keys) {
            conn->close();
            co_return;
        }
        bool member = is_member(conn->peer_key());
        if (member) {
            // What the sender holds: its LAN advertisements under older keys are forgeries.
            auto& seen = seen_generation_[conn->peer_key()];
            auto& seen_key = seen_key_[conn->peer_key()];
            for (const auto& k : keys.value()) {
                seen = std::max(seen, k.generation);
                seen_key = std::max(seen_key, DiscoveryKeyId{k.generation, k.author});
            }
            auto& held = drive_.discovery_keys();
            auto before = held.current_id();
            if (held.merge(keys.value(), member_keys(drive_.membership().view()))) {
                (void)drive_.save_discovery_keys();
                if (held.current_id() != before) on_discovery_key_changed();
            }
        }
        for (auto& k : keys.value()) core::secure_zero(k.key);
        if (!member) co_return;
        frame = co_await wire::read_frame(*stream, kMaxDiscoveryKeysFrame);
        if (!*alive || !frame) co_return;
    }
}

asio::awaitable<void> Service::farewell(std::shared_ptr<transport::Connection> conn) {
    auto alive = alive_;
    // Closed on time even if the removed device withholds stream credit or flow control.
    auto deadline = std::make_shared<asio::steady_timer>(io_, std::chrono::seconds(2));
    deadline->async_wait([conn](const std::error_code& ec) {
        if (!ec) conn->close();
    });
    auto stream = co_await conn->open_stream();
    if (*alive && stream && co_await stream.value()->write(encode_membership({}))) {
        // Only the entries removing it: their authors are members it vouched for, so it can
        // verify them, and it learns nothing else about the drive.
        std::vector<MembershipEntry> removals;
        for (auto& e : drive_.membership().entries()) {
            if (e.kind == MembershipEntry::Kind::remove && e.subject == conn->peer_key()) {
                removals.push_back(std::move(e));
                if (removals.size() == kMaxMembershipBatch) break;
            }
        }
        if (*alive && co_await stream.value()->write(encode_membership(removals), true)) {
            co_await sleep_for(std::chrono::milliseconds(500));  // let it arrive before closing
        }
    }
    if (*alive) conn->close();
}

asio::awaitable<void> Service::push_membership(std::shared_ptr<transport::Connection> conn) {
    auto alive = alive_;
    auto stream = co_await conn->open_stream();
    if (!*alive || !stream) co_return;
    // An empty batch first says what the stream is for, within the small first-frame limit.
    if (!co_await stream.value()->write(encode_membership({}))) co_return;
    std::uint64_t sent = 0;
    while (*alive && conn->is_open() && is_member(conn->peer_key())) {
        auto& membership = drive_.membership();
        if (membership.last_sequence() <= sent) {
            co_await sleep_for(options_.sync_poll);
            continue;
        }
        std::uint64_t last = sent;
        auto batch = membership.since(sent, kMaxMembershipBatch, last);
        if (batch.empty()) {
            sent = membership.last_sequence();
            continue;
        }
        if (!co_await stream.value()->write(encode_membership(batch))) co_return;
        if (!*alive) co_return;
        sent = last;
    }
}

asio::awaitable<void> Service::receive_membership(std::shared_ptr<transport::Connection> conn,
                                                  std::shared_ptr<transport::Stream> stream,
                                                  wire::Frame first) {
    auto alive = alive_;
    auto frame = core::Result<wire::Frame>(std::move(first));
    for (;;) {
        if (frame.value().type != wire::FrameType::membership) {
            conn->close();
            co_return;
        }
        auto entries = decode_membership(frame.value().payload);
        if (!entries) {
            conn->close();
            co_return;
        }
        if (!is_member(conn->peer_key())) co_return;
        if (drive_.membership().merge(entries.value()) > 0) (void)refresh_membership(true);
        frame = co_await wire::read_frame(*stream, kMaxMembershipFrame);
        if (!*alive || !frame) co_return;
    }
}

asio::awaitable<core::Result<JoinResult>> request_join(transport::Endpoint& endpoint,
                                                       const Invite& invite, const Join& join) {
    std::optional<Error> last_error;
    for (const auto& address : invite.addresses) {
        // The inviter's key is pinned here, so only the device that made the code can answer.
        auto conn = co_await endpoint.connect(address, invite.inviter);
        if (!conn) {
            last_error = conn.error();
            continue;
        }
        auto stream = co_await conn.value()->open_stream();
        if (!stream) {
            last_error = stream.error();
            continue;
        }
        auto request = encode_join(join);
        if (auto st = co_await stream.value()->write(request, true); !st) {
            last_error = st.error();
            continue;
        }
        auto reply = co_await wire::read_frame(*stream.value(), kMaxDriveInfo);
        conn.value()->close();
        if (!reply) {
            last_error = reply.error();
            continue;
        }
        if (reply.value().type != wire::FrameType::drive_info) {
            co_return Error{Errc::permission, "the invite was rejected (expired or already used)"};
        }
        auto info = decode_drive_info(reply.value().payload);
        core::secure_zero(reply.value().payload);
        if (!info) co_return info.error();
        co_return JoinResult{std::move(info).value(), address};
    }
    co_return last_error ? *last_error
                         : Error{Errc::not_found, "the invite has no reachable address"};
}

}  // namespace ghostdrive::daemon
