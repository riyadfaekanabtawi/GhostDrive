#include "ghostdrive/daemon/service.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <charconv>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "async.hpp"
#include "ghostdrive/discovery/lan_id.hpp"
#include "ghostdrive/discovery/rendezvous_codec.hpp"
#include "ghostdrive/transport/demux.hpp"
#ifdef GHOSTDRIVE_HAVE_DNSSD
#include "ghostdrive/discovery/lan_discovery.hpp"
#endif
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
// The first frame of any stream is small: a GetChunk, Hello or Join.
constexpr std::uint32_t kMaxFirstFrame = 4096;
constexpr std::uint32_t kMaxDriveInfo = 8 * 1024 * 1024;  // the largest membership log fits
constexpr std::uint32_t kMaxMembershipFrame = 256 * 1024;
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
        });
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
#ifdef GHOSTDRIVE_HAVE_DNSSD
    if (options_.lan_discovery) {
        auto lan = discovery::LanDiscovery::start(
            io_, port(),
            [this] {
                return discovery::lan_id(drive_.secret(), drive_.key().public_key(),
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
    auto key = discovery::match_lan_id(sighting.id, drive_.secret(), members,
                                       std::chrono::system_clock::now());
    if (!key) return;  // another drive, or a stale or forged advertisement
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
            auto connected = co_await endpoint_->connect(server, config->key);
            if (!*alive || stopped_) co_return;
            if (connected) conn = std::move(connected).value();
        }
        if (!conn) {
            co_await sleep_for(backoff);
            backoff = std::min<std::chrono::milliseconds>(backoff * 2, std::chrono::seconds(30));
            continue;
        }

        auto group = discovery::rendezvous_group(drive_.secret());
        auto control = co_await conn->open_stream();
        bool registered = false;
        if (control && group) {
            auto request = discovery::encode(discovery::Register{group.value()});
            if (co_await control.value()->write(request)) {
                auto reply = co_await wire::read_frame(*control.value(), 64);
                if (reply && reply.value().type == wire::FrameType::rendezvous_registered) {
                    if (auto r = discovery::decode_registered(reply.value().payload)) {
                        observed_ = r.value().observed;
                        // Relayed packets are accepted only with our token and only from
                        // members; QUIC then authenticates the member itself.
                        endpoint_->set_relay(server, r.value().relay_token,
                                             [this](const core::PublicKey& key) { return is_member(key); });
                        registered = true;
                    }
                }
            }
        }
        if (!*alive) co_return;
        if (registered) {
            backoff = std::chrono::milliseconds(1'000);
            rendezvous_conn_ = conn;
            asio::co_spawn(io_, request_peers(conn, control.value()), asio::detached);
            for (;;) {
                auto frame = co_await wire::read_frame(*control.value(), 64);
                if (!*alive) co_return;
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

asio::awaitable<void> Service::request_peers(std::shared_ptr<transport::Connection> conn,
                                             std::shared_ptr<transport::Stream> control) {
    auto alive = alive_;
    while (*alive && !stopped_ && conn->is_open()) {
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
        co_await sleep_for(options_.rendezvous_interval);
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
    if (rendezvous_conn_) rendezvous_conn_->close();
    for (auto& [key, conn] : live_) conn->close();
    live_.clear();
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
    auto view = drive_.membership().view();
    if (view.removed_by) {
        auto it = view.members.find(*view.removed_by);
        out.removed_by = it != view.members.end() ? it->second : core::fingerprint(*view.removed_by);
    }
    if (auto group = discovery::rendezvous_group(drive_.secret())) {
        out.rendezvous_group = core::to_hex(group.value());
    }
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
    }
    asio::co_spawn(io_, push_index(conn), asio::detached);
    asio::co_spawn(io_, push_membership(conn), asio::detached);
    for (;;) {
        auto stream = co_await conn->accept_stream();
        if (!*alive) co_return;
        if (!stream) break;  // connection closed
        // A device removed while connected gets nothing more.
        if (!is_member(key)) continue;
        asio::co_spawn(io_, handle_stream(conn, std::move(stream).value()), asio::detached);
    }
    member_conns_.erase(conn);
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
                auto records = drive->index().records_since(after, kBatchRecords);
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

    for (;;) {
        auto frame = co_await wire::read_frame(*stream);
        if (!*alive || !frame) co_return;
        if (frame.value().type != wire::FrameType::index_batch) {
            conn->close();
            co_return;
        }
        auto entries = decode_index_batch(frame.value().payload);
        if (!entries) {
            conn->close();
            co_return;
        }
        if (!is_member(peer)) co_return;
        auto applied = co_await detail::run_on(
            disk_, [drive, fs, peer, hello, entries = std::move(entries).value()]() -> Status {
                auto& index = drive->index();
                std::uint64_t last = 0;
                Status failure;
                for (const auto& e : entries) {
                    // apply_remote validates the record (paths, block size, clocks) first.
                    auto outcome = index.apply_remote(e.record, drive->clock());
                    if (!outcome && outcome.error().code != Errc::invalid_argument) {
                        // A database failure: stop here so the next exchange resends this record.
                        failure = outcome.error();
                        break;
                    }
                    // A rejected record will never become valid; skip past it.
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
                if (last > 0) {
                    if (auto st = index.set_peer_progress(peer, hello.index_id, last); !st) {
                        return st;
                    }
                }
                if (!failure) return failure;
                auto refreshed = fs->refresh(index);
                if (!refreshed) return refreshed.error();
                return {};
            });
        if (!*alive) co_return;
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
    auto unpinned = drive_.apply_membership();
    if (!unpinned) return unpinned.error();
    for (const auto& key : unpinned.value()) {
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
    return {};
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
