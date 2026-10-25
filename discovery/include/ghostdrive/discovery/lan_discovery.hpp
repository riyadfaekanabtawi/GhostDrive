#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/ip/udp.hpp>
#ifndef _WIN32
#include <asio/posix/stream_descriptor.hpp>
#endif
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#ifdef GHOSTDRIVE_LAN_WINDNS
#include <optional>
#include <set>
#include <utility>
#include <vector>
#endif

#include "ghostdrive/core/result.hpp"

#ifdef GHOSTDRIVE_LAN_AVAHI
struct AvahiAddress;
struct AvahiClient;
struct AvahiEntryGroup;
struct AvahiServiceBrowser;
struct AvahiStringList;
#elif !defined(GHOSTDRIVE_LAN_WINDNS)
typedef struct _DNSServiceRef_t* DNSServiceRef;
#endif

namespace ghostdrive::discovery {

inline constexpr const char* kServiceType = "_ghostdrive._udp";

// One advertisement seen on the LAN: who it claims to be (the TXT "id") and where.
struct LanSighting {
    std::string instance;
    std::string id;
    asio::ip::udp::endpoint endpoint;
};

// Advertises this device and browses for others with DNS-SD over mDNS, through the system's
// responder (mDNSResponder, avahi-daemon through avahi-client on Linux, or the DNS Client
// service through windns.h on Windows). An advertisement is only a hint: dialing it still pins
// the member's key, so a forged one costs one dial. Use from one executor.
class LanDiscovery {
public:
    using IdSource = std::function<std::string()>;
    using SightingHandler = std::function<void(const LanSighting&)>;

    // `current_id` is called now and again whenever the epoch may have turned, to refresh the
    // advertised ID. `on_sighting` runs on the executor for each resolved IPv4 address.
    static core::Result<std::unique_ptr<LanDiscovery>> start(asio::any_io_executor executor,
                                                             std::uint16_t port,
                                                             IdSource current_id,
                                                             SightingHandler on_sighting);
    LanDiscovery(const LanDiscovery&) = delete;
    LanDiscovery& operator=(const LanDiscovery&) = delete;
    ~LanDiscovery();

    const std::string& instance_name() const noexcept { return instance_; }
    // Advertises the current ID now, for when it changed other than by the epoch turning.
    void refresh();

    struct Op;
    struct Private {};
    LanDiscovery(Private, asio::any_io_executor executor, std::uint16_t port, IdSource current_id,
                 SightingHandler on_sighting);

    // Called from the C callbacks.
#ifdef GHOSTDRIVE_LAN_AVAHI
    void on_client_state(AvahiClient* client, int state);
    void on_group_state(int state);
    void on_browse(int interface, int protocol, int event, const char* name, const char* type,
                   const char* domain, unsigned flags);
    void on_resolve(Op* op, int event, const AvahiAddress* address, std::uint16_t port,
                    AvahiStringList* txt);
#elif defined(GHOSTDRIVE_LAN_WINDNS)
    enum class Registration { pending, registered, gone, failed };
    // Service instance names seen, each with false for a goodbye.
    void on_browse(std::vector<std::pair<std::string, bool>> names);
    void on_resolve(std::uint64_t token, bool ok, std::string id, std::uint16_t port,
                    std::optional<asio::ip::address_v4> address);
    void on_register(std::uint64_t token, Registration state);
#else
    void on_browse(std::uint32_t flags, std::uint32_t interface, int error, const char* name,
                   const char* type, const char* domain);
    void on_resolve(Op* op, int error, const char* host, std::uint16_t port_be,
                    std::uint16_t txt_len, const unsigned char* txt);
    void on_address(Op* op, std::uint32_t flags, int error, const void* address);
#endif

private:
    core::Status begin();
#ifdef GHOSTDRIVE_LAN_AVAHI
    void add_service();
#elif defined(GHOSTDRIVE_LAN_WINDNS)
    core::Status register_service();
    void leave();
#else
    void wait_readable();
#endif
    void refresh_id();
    void finish(Op* op);
    void expire_ops();

    asio::any_io_executor executor_;
    std::uint16_t port_;
    IdSource current_id_;
    SightingHandler on_sighting_;
    std::string instance_;
    std::string advertised_id_;

#ifdef GHOSTDRIVE_LAN_AVAHI
    // Avahi's event loop, run on the executor (lan_discovery_avahi.cpp).
    struct Poll;
    std::unique_ptr<Poll> poll_;
    AvahiClient* client_ = nullptr;
    AvahiEntryGroup* group_ = nullptr;
    AvahiServiceBrowser* browser_ = nullptr;
#elif defined(GHOSTDRIVE_LAN_WINDNS)
    // Requests handed to dnsapi, by token (lan_discovery_windns.cpp).
    std::wstring host_;
    std::vector<std::uint64_t> calls_;
    std::uint64_t registration_ = 0;
    Registration registration_state_ = Registration::gone;
    std::string registered_id_;
    std::set<std::string> known_;  // advertisements resolved or being resolved
#else
    DNSServiceRef main_ = nullptr;
    DNSServiceRef register_ = nullptr;
    DNSServiceRef browse_ = nullptr;
#ifndef _WIN32
    asio::posix::stream_descriptor descriptor_;
#endif
#endif
    asio::steady_timer refresh_timer_;
    std::list<std::unique_ptr<Op>> ops_;

    // Rate limit for resolutions, so a flood of advertisements cannot keep us busy.
    std::chrono::steady_clock::time_point window_start_{};
    int window_count_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace ghostdrive::discovery
