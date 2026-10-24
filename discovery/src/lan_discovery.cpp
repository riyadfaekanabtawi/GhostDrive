#include "ghostdrive/discovery/lan_discovery.hpp"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <netinet/in.h>

#include <cstring>

#include "ghostdrive/core/bytes.hpp"

namespace ghostdrive::discovery {

using core::Errc;
using core::Error;

namespace {

constexpr std::size_t kMaxOps = 64;        // resolutions in flight
constexpr int kMaxResolvesPerWindow = 32;  // per kRateWindow
constexpr std::chrono::seconds kRateWindow{10};
constexpr std::chrono::seconds kOpTimeout{10};
constexpr std::chrono::seconds kRefreshEvery{60};

core::Bytes txt_record(const std::string& id) {
    core::Bytes txt;
    for (const std::string& entry : {std::string("v=1"), "id=" + id}) {
        txt.push_back(static_cast<std::byte>(entry.size()));
        auto b = core::as_bytes(entry);
        txt.insert(txt.end(), b.begin(), b.end());
    }
    return txt;
}

}  // namespace

// One advertisement being resolved: DNSServiceResolve, then DNSServiceGetAddrInfo.
struct LanDiscovery::Op {
    LanDiscovery* owner = nullptr;
    std::string instance;
    std::string id;
    std::uint16_t port = 0;
    DNSServiceRef ref = nullptr;
    std::chrono::steady_clock::time_point started;
};

namespace {

void DNSSD_API browse_reply(DNSServiceRef, DNSServiceFlags flags, uint32_t interface,
                            DNSServiceErrorType error, const char* name, const char* type,
                            const char* domain, void* context) {
    static_cast<LanDiscovery*>(context)->on_browse(flags, interface, error, name, type, domain);
}

void DNSSD_API resolve_reply(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error,
                             const char*, const char* host, uint16_t port, uint16_t txt_len,
                             const unsigned char* txt, void* context) {
    auto* op = static_cast<LanDiscovery::Op*>(context);
    op->owner->on_resolve(op, error, host, port, txt_len, txt);
}

void DNSSD_API address_reply(DNSServiceRef, DNSServiceFlags flags, uint32_t,
                             DNSServiceErrorType error, const char*, const sockaddr* address,
                             uint32_t, void* context) {
    auto* op = static_cast<LanDiscovery::Op*>(context);
    op->owner->on_address(op, flags, error, address);
}

void DNSSD_API register_reply(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType, const char*,
                              const char*, const char*, void*) {}

}  // namespace

LanDiscovery::LanDiscovery(Private, asio::any_io_executor executor, std::uint16_t port,
                           IdSource current_id, SightingHandler on_sighting)
    : executor_(executor),
      port_(port),
      current_id_(std::move(current_id)),
      on_sighting_(std::move(on_sighting)),
      descriptor_(executor),
      refresh_timer_(executor) {}

core::Result<std::unique_ptr<LanDiscovery>> LanDiscovery::start(asio::any_io_executor executor,
                                                                std::uint16_t port,
                                                                IdSource current_id,
                                                                SightingHandler on_sighting) {
    auto lan = std::make_unique<LanDiscovery>(Private{}, executor, port, std::move(current_id),
                                              std::move(on_sighting));
    if (auto st = lan->begin(); !st) return st.error();
    return lan;
}

core::Status LanDiscovery::begin() {
    // A random instance name, so the name itself identifies nothing.
    std::array<std::byte, 8> random{};
    if (auto st = core::random_bytes(random); !st) return st;
    instance_ = "ghostdrive-" + core::to_hex(random);
    advertised_id_ = current_id_();

    if (DNSServiceCreateConnection(&main_) != kDNSServiceErr_NoError) {
        main_ = nullptr;
        return Error{Errc::io, "cannot reach the system mDNS responder"};
    }
    auto txt = txt_record(advertised_id_);
    register_ = main_;
    auto err = DNSServiceRegister(&register_, kDNSServiceFlagsShareConnection,
                                  kDNSServiceInterfaceIndexAny, instance_.c_str(), kServiceType,
                                  nullptr, nullptr, htons(port_), static_cast<uint16_t>(txt.size()),
                                  txt.data(), register_reply, this);
    if (err != kDNSServiceErr_NoError) {
        register_ = nullptr;
        return Error{Errc::io, "mDNS registration failed (" + std::to_string(err) + ")"};
    }
    browse_ = main_;
    err = DNSServiceBrowse(&browse_, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny,
                           kServiceType, nullptr, browse_reply, this);
    if (err != kDNSServiceErr_NoError) {
        browse_ = nullptr;
        return Error{Errc::io, "mDNS browse failed (" + std::to_string(err) + ")"};
    }

    std::error_code ec;
    descriptor_.assign(DNSServiceRefSockFD(main_), ec);
    if (ec) return Error{Errc::io, "mDNS socket: " + ec.message()};
    wait_readable();
    refresh_id();
    return {};
}

LanDiscovery::~LanDiscovery() {
    *alive_ = false;
    std::error_code ec;
    refresh_timer_.cancel();
    // The descriptor belongs to dns_sd; give it back before deallocating the connection.
    descriptor_.cancel(ec);
    if (descriptor_.is_open()) (void)descriptor_.release();
    for (auto& op : ops_) {
        if (op->ref) DNSServiceRefDeallocate(op->ref);
    }
    if (browse_) DNSServiceRefDeallocate(browse_);
    if (register_) DNSServiceRefDeallocate(register_);
    if (main_) DNSServiceRefDeallocate(main_);
}

void LanDiscovery::wait_readable() {
    descriptor_.async_wait(asio::posix::stream_descriptor::wait_read,
                           [this, alive = alive_](const std::error_code& ec) {
                               if (!*alive || ec) return;
                               if (DNSServiceProcessResult(main_) != kDNSServiceErr_NoError) {
                                   return;  // the responder went away; discovery stops
                               }
                               wait_readable();
                           });
}

void LanDiscovery::refresh_id() {
    refresh_timer_.expires_after(kRefreshEvery);
    refresh_timer_.async_wait([this, alive = alive_](const std::error_code& ec) {
        if (!*alive || ec) return;
        auto id = current_id_();
        if (id != advertised_id_) {
            auto txt = txt_record(id);
            if (DNSServiceUpdateRecord(register_, nullptr, 0, static_cast<uint16_t>(txt.size()),
                                       txt.data(), 0) == kDNSServiceErr_NoError) {
                advertised_id_ = id;
            }
        }
        expire_ops();
        refresh_id();
    });
}

void LanDiscovery::on_browse(std::uint32_t flags, std::uint32_t interface, int error,
                             const char* name, const char* type, const char* domain) {
    if (error != kDNSServiceErr_NoError || (flags & kDNSServiceFlagsAdd) == 0) return;
    if (name == nullptr || instance_ == name) return;  // ourselves
    expire_ops();
    if (ops_.size() >= kMaxOps) return;
    auto now = std::chrono::steady_clock::now();
    if (now - window_start_ > kRateWindow) {
        window_start_ = now;
        window_count_ = 0;
    }
    if (++window_count_ > kMaxResolvesPerWindow) return;

    auto op = std::make_unique<Op>();
    op->owner = this;
    op->instance = name;
    op->started = now;
    op->ref = main_;
    if (DNSServiceResolve(&op->ref, kDNSServiceFlagsShareConnection, interface, name, type, domain,
                          resolve_reply, op.get()) != kDNSServiceErr_NoError) {
        return;
    }
    ops_.push_back(std::move(op));
}

void LanDiscovery::on_resolve(Op* op, int error, const char* host, std::uint16_t port_be,
                              std::uint16_t txt_len, const unsigned char* txt) {
    if (error != kDNSServiceErr_NoError || host == nullptr) {
        finish(op);
        return;
    }
    std::uint8_t len = 0;
    const void* id = TXTRecordGetValuePtr(txt_len, txt, "id", &len);
    if (id == nullptr || len == 0 || len > 64) {
        finish(op);
        return;
    }
    op->id.assign(static_cast<const char*>(id), len);
    op->port = ntohs(port_be);
    // Resolving is done; the same Op now waits for the host's address.
    DNSServiceRefDeallocate(op->ref);
    op->ref = main_;
    if (DNSServiceGetAddrInfo(&op->ref, kDNSServiceFlagsShareConnection, 0,
                              kDNSServiceProtocol_IPv4, host, address_reply,
                              op) != kDNSServiceErr_NoError) {
        op->ref = nullptr;
        finish(op);
    }
}

void LanDiscovery::on_address(Op* op, std::uint32_t flags, int error, const void* address) {
    if (error == kDNSServiceErr_NoError && (flags & kDNSServiceFlagsAdd) != 0 &&
        address != nullptr) {
        const auto* sa = static_cast<const sockaddr*>(address);
        if (sa->sa_family == AF_INET) {
            const auto* sin = static_cast<const sockaddr_in*>(address);
            asio::ip::address_v4 v4(ntohl(sin->sin_addr.s_addr));
            LanSighting sighting{op->instance, op->id, asio::ip::udp::endpoint(v4, op->port)};
            if (on_sighting_) on_sighting_(sighting);
        }
    }
    if ((flags & kDNSServiceFlagsMoreComing) == 0) finish(op);
}

void LanDiscovery::finish(Op* op) {
    // Deallocating a shared-connection ref inside its own callback is allowed by dns_sd.
    if (op->ref) DNSServiceRefDeallocate(op->ref);
    op->ref = nullptr;
    ops_.remove_if([op](const auto& o) { return o.get() == op; });
}

void LanDiscovery::expire_ops() {
    auto now = std::chrono::steady_clock::now();
    ops_.remove_if([now](const auto& o) {
        if (now - o->started < kOpTimeout) return false;
        if (o->ref) DNSServiceRefDeallocate(o->ref);
        return true;
    });
}

}  // namespace ghostdrive::discovery
