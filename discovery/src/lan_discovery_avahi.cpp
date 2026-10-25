// LanDiscovery through avahi-daemon with avahi-client, for Linux. Avahi's event loop is an
// AvahiPoll implemented on Asio, so every Avahi callback runs on the executor; no thread is added.
#include <arpa/inet.h>
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/strlst.h>
#include <avahi-common/watch.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/discovery/lan_discovery.hpp"

namespace ghostdrive::discovery {
namespace {
struct EventLoop;
}
}  // namespace ghostdrive::discovery

// Avahi leaves these two types to the AvahiPoll provider. Avahi owns each one between its _new
// and _free calls; the shared_ptr in EventLoop keeps it alive for Asio handlers still queued.
struct AvahiWatch {
    ghostdrive::discovery::EventLoop* loop;
    int fd;  // as Avahi gave it; the descriptor holds a duplicate
    asio::posix::stream_descriptor descriptor;
    AvahiWatchCallback callback;
    void* userdata;
    int wanted = 0;
    int happened = 0;
    bool reading = false;
    bool writing = false;
    bool dead = false;
};

struct AvahiTimeout {
    ghostdrive::discovery::EventLoop* loop;
    asio::steady_timer timer;
    AvahiTimeoutCallback callback;
    void* userdata;
    unsigned generation = 0;
    bool dead = false;
};

namespace ghostdrive::discovery {

using core::Errc;
using core::Error;

namespace {

constexpr std::size_t kMaxOps = 64;        // resolutions in flight
constexpr int kMaxResolvesPerWindow = 32;  // per kRateWindow
constexpr std::chrono::seconds kRateWindow{10};
constexpr std::chrono::seconds kOpTimeout{10};
constexpr std::chrono::seconds kRefreshEvery{60};
constexpr auto kNoPublishFlags = static_cast<AvahiPublishFlags>(0);
constexpr auto kNoLookupFlags = static_cast<AvahiLookupFlags>(0);

void arm(const std::shared_ptr<AvahiWatch>& w);

void fire(const std::shared_ptr<AvahiWatch>& w, int event, const std::error_code& ec) {
    if (w->dead || ec == asio::error::operation_aborted) return;
    if (ec) event = AVAHI_WATCH_ERR;
    // A wait may outlive a watch_update that dropped its event; it is then ignored.
    if (ec || (w->wanted & event) != 0) {
        w->happened = event;
        w->callback(w.get(), w->fd, static_cast<AvahiWatchEvent>(event), w->userdata);
        w->happened = 0;
    }
    // Avahi watches are level-triggered: wait again until the event is no longer wanted.
    if (!w->dead && !ec) arm(w);
}

void arm(const std::shared_ptr<AvahiWatch>& w) {
    if ((w->wanted & AVAHI_WATCH_IN) != 0 && !w->reading) {
        w->reading = true;
        w->descriptor.async_wait(asio::posix::descriptor_base::wait_read,
                                 [w](const std::error_code& ec) {
                                     w->reading = false;
                                     fire(w, AVAHI_WATCH_IN, ec);
                                 });
    }
    if ((w->wanted & AVAHI_WATCH_OUT) != 0 && !w->writing) {
        w->writing = true;
        w->descriptor.async_wait(asio::posix::descriptor_base::wait_write,
                                 [w](const std::error_code& ec) {
                                     w->writing = false;
                                     fire(w, AVAHI_WATCH_OUT, ec);
                                 });
    }
}

void set_timeout(const std::shared_ptr<AvahiTimeout>& t, const timeval* tv) {
    ++t->generation;
    t->timer.cancel();
    if (tv == nullptr) return;
    // Avahi gives absolute wall-clock times (gettimeofday); wait the remaining time.
    timeval now{};
    gettimeofday(&now, nullptr);
    auto delay = std::chrono::seconds(tv->tv_sec - now.tv_sec) +
                 std::chrono::microseconds(tv->tv_usec - now.tv_usec);
    t->timer.expires_after(std::max(delay, std::chrono::microseconds::zero()));
    t->timer.async_wait([t, generation = t->generation](const std::error_code& ec) {
        if (t->dead || ec || generation != t->generation) return;
        t->callback(t.get(), t->userdata);
    });
}

// The AvahiPoll handed to avahi-client.
struct EventLoop {
    explicit EventLoop(asio::any_io_executor ex) : executor(std::move(ex)) {
        api.userdata = this;
        api.watch_new = &watch_new;
        api.watch_update = &watch_update;
        api.watch_get_events = &watch_get_events;
        api.watch_free = &watch_free;
        api.timeout_new = &timeout_new;
        api.timeout_update = &timeout_update;
        api.timeout_free = &timeout_free;
    }
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    ~EventLoop() {
        // avahi_client_free has freed everything by now; this only covers a failed start.
        for (auto& w : watches) {
            w->dead = true;
            std::error_code ec;
            w->descriptor.cancel(ec);
        }
        for (auto& t : timeouts) {
            t->dead = true;
            t->timer.cancel();
        }
    }

    static AvahiWatch* watch_new(const AvahiPoll* api, int fd, AvahiWatchEvent event,
                                 AvahiWatchCallback callback, void* userdata) {
        auto* loop = static_cast<EventLoop*>(api->userdata);
        // D-Bus watches one socket twice (read and write), and Asio registers a descriptor only
        // once, so each watch gets a duplicate of its own.
        int own = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (own < 0) return nullptr;
        auto w = std::make_shared<AvahiWatch>(AvahiWatch{
            loop, fd, asio::posix::stream_descriptor(loop->executor), callback, userdata});
        std::error_code ec;
        w->descriptor.assign(own, ec);
        if (ec) {
            ::close(own);
            return nullptr;
        }
        w->wanted = event;
        loop->watches.push_back(w);
        arm(w);
        return w.get();
    }

    static void watch_update(AvahiWatch* w, AvahiWatchEvent event) {
        w->wanted = event;
        auto it = std::find_if(w->loop->watches.begin(), w->loop->watches.end(),
                               [w](const auto& p) { return p.get() == w; });
        if (it != w->loop->watches.end()) arm(*it);
    }

    static AvahiWatchEvent watch_get_events(AvahiWatch* w) {
        return static_cast<AvahiWatchEvent>(w->happened);
    }

    static void watch_free(AvahiWatch* w) {
        w->dead = true;
        std::error_code ec;
        w->descriptor.cancel(ec);
        std::erase_if(w->loop->watches, [w](const auto& p) { return p.get() == w; });
    }

    static AvahiTimeout* timeout_new(const AvahiPoll* api, const timeval* tv,
                                     AvahiTimeoutCallback callback, void* userdata) {
        auto* loop = static_cast<EventLoop*>(api->userdata);
        auto t = std::make_shared<AvahiTimeout>(
            AvahiTimeout{loop, asio::steady_timer(loop->executor), callback, userdata});
        loop->timeouts.push_back(t);
        set_timeout(t, tv);
        return t.get();
    }

    static void timeout_update(AvahiTimeout* t, const timeval* tv) {
        auto it = std::find_if(t->loop->timeouts.begin(), t->loop->timeouts.end(),
                               [t](const auto& p) { return p.get() == t; });
        if (it != t->loop->timeouts.end()) set_timeout(*it, tv);
    }

    static void timeout_free(AvahiTimeout* t) {
        t->dead = true;
        t->timer.cancel();
        std::erase_if(t->loop->timeouts, [t](const auto& p) { return p.get() == t; });
    }

    asio::any_io_executor executor;
    AvahiPoll api{};
    std::vector<std::shared_ptr<AvahiWatch>> watches;
    std::vector<std::shared_ptr<AvahiTimeout>> timeouts;
};

}  // namespace

struct LanDiscovery::Poll : EventLoop {
    using EventLoop::EventLoop;
};

// One advertisement being resolved (SRV, TXT and the host's IPv4 address in one resolver).
struct LanDiscovery::Op {
    LanDiscovery* owner = nullptr;
    std::string instance;
    AvahiServiceResolver* resolver = nullptr;
    std::chrono::steady_clock::time_point started;
};

namespace {

void client_reply(AvahiClient* client, AvahiClientState state, void* userdata) {
    static_cast<LanDiscovery*>(userdata)->on_client_state(client, state);
}

void group_reply(AvahiEntryGroup*, AvahiEntryGroupState state, void* userdata) {
    static_cast<LanDiscovery*>(userdata)->on_group_state(state);
}

void browse_reply(AvahiServiceBrowser*, AvahiIfIndex interface, AvahiProtocol protocol,
                  AvahiBrowserEvent event, const char* name, const char* type, const char* domain,
                  AvahiLookupResultFlags flags, void* userdata) {
    static_cast<LanDiscovery*>(userdata)->on_browse(interface, protocol, event, name, type, domain,
                                                    flags);
}

void resolve_reply(AvahiServiceResolver*, AvahiIfIndex, AvahiProtocol, AvahiResolverEvent event,
                   const char*, const char*, const char*, const char*, const AvahiAddress* address,
                   uint16_t port, AvahiStringList* txt, AvahiLookupResultFlags, void* userdata) {
    auto* op = static_cast<LanDiscovery::Op*>(userdata);
    op->owner->on_resolve(op, event, address, port, txt);
}

core::Result<std::string> random_instance() {
    // A random instance name, so the name itself identifies nothing.
    std::array<std::byte, 8> random{};
    if (auto st = core::random_bytes(random); !st) return st.error();
    return "ghostdrive-" + core::to_hex(random);
}

}  // namespace

LanDiscovery::LanDiscovery(Private, asio::any_io_executor executor, std::uint16_t port,
                           IdSource current_id, SightingHandler on_sighting)
    : executor_(executor),
      port_(port),
      current_id_(std::move(current_id)),
      on_sighting_(std::move(on_sighting)),
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
    auto name = random_instance();
    if (!name) return name.error();
    instance_ = std::move(name).value();
    advertised_id_ = current_id_();

    poll_ = std::make_unique<Poll>(executor_);
    int error = 0;
    // Without AVAHI_CLIENT_NO_FAIL this fails at once when avahi-daemon is not running. The
    // state callback runs inside the call and may already register the service.
    auto* client =
        avahi_client_new(&poll_->api, static_cast<AvahiClientFlags>(0), client_reply, this, &error);
    if (client == nullptr) {
        client_ = nullptr;
        group_ = nullptr;  // freed along with the client
        return Error{Errc::io, std::string("cannot reach avahi-daemon: ") + avahi_strerror(error)};
    }
    client_ = client;
    if (avahi_client_get_state(client_) == AVAHI_CLIENT_S_RUNNING &&
        (group_ == nullptr || avahi_entry_group_is_empty(group_) != 0)) {
        return Error{Errc::io, std::string("mDNS registration failed: ") +
                                   avahi_strerror(avahi_client_errno(client_))};
    }
    browser_ = avahi_service_browser_new(client_, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, kServiceType,
                                         nullptr, kNoLookupFlags, browse_reply, this);
    if (browser_ == nullptr) {
        return Error{Errc::io, std::string("mDNS browse failed: ") +
                                   avahi_strerror(avahi_client_errno(client_))};
    }
    refresh_id();
    return {};
}

LanDiscovery::~LanDiscovery() {
    *alive_ = false;
    refresh_timer_.cancel();
    for (auto& op : ops_) {
        if (op->resolver) avahi_service_resolver_free(op->resolver);
    }
    if (browser_) avahi_service_browser_free(browser_);
    if (group_) avahi_entry_group_free(group_);
    if (client_) avahi_client_free(client_);
}

void LanDiscovery::on_client_state(AvahiClient* client, int state) {
    client_ = client;  // the first call comes from inside avahi_client_new
    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            add_service();
            break;
        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
            // The host name is changing; the service goes back up once the daemon runs again.
            if (group_) avahi_entry_group_reset(group_);
            break;
        default:
            break;  // AVAHI_CLIENT_FAILURE: avahi-daemon went away and discovery stops
    }
}

void LanDiscovery::add_service() {
    if (group_ == nullptr) group_ = avahi_entry_group_new(client_, group_reply, this);
    if (group_ == nullptr || avahi_entry_group_is_empty(group_) == 0) return;
    auto id = "id=" + advertised_id_;
    if (avahi_entry_group_add_service(group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, kNoPublishFlags,
                                      instance_.c_str(), kServiceType, nullptr, nullptr, port_,
                                      "v=1", id.c_str(),
                                      static_cast<const char*>(nullptr)) != AVAHI_OK) {
        return;
    }
    (void)avahi_entry_group_commit(group_);
}

void LanDiscovery::on_group_state(int state) {
    // The first call comes from inside avahi_entry_group_new, before group_ is set.
    if (state != AVAHI_ENTRY_GROUP_COLLISION || group_ == nullptr) return;
    // Another host took the random name; pick a new one.
    auto name = random_instance();
    if (!name) return;
    instance_ = std::move(name).value();
    avahi_entry_group_reset(group_);
    add_service();
}

void LanDiscovery::refresh() {
    auto id = current_id_();
    if (id == advertised_id_) return;
    if (group_ == nullptr || avahi_entry_group_is_empty(group_) != 0) {
        advertised_id_ = id;  // add_service will advertise it
        return;
    }
    auto entry = "id=" + id;
    if (avahi_entry_group_update_service_txt(group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                             kNoPublishFlags, instance_.c_str(), kServiceType,
                                             nullptr, "v=1", entry.c_str(),
                                             static_cast<const char*>(nullptr)) == AVAHI_OK) {
        advertised_id_ = id;
    }
}

void LanDiscovery::refresh_id() {
    refresh_timer_.expires_after(kRefreshEvery);
    refresh_timer_.async_wait([this, alive = alive_](const std::error_code& ec) {
        if (!*alive || ec) return;
        refresh();
        expire_ops();
        refresh_id();
    });
}

void LanDiscovery::on_browse(int interface, int protocol, int event, const char* name,
                             const char* type, const char* domain, unsigned flags) {
    if (event != AVAHI_BROWSER_NEW || name == nullptr) return;
    if ((flags & AVAHI_LOOKUP_RESULT_OUR_OWN) != 0 || instance_ == name) return;  // ourselves
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
    op->resolver =
        avahi_service_resolver_new(client_, interface, protocol, name, type, domain,
                                   AVAHI_PROTO_INET, kNoLookupFlags, resolve_reply, op.get());
    if (op->resolver == nullptr) return;
    ops_.push_back(std::move(op));
}

void LanDiscovery::on_resolve(Op* op, int event, const AvahiAddress* address, std::uint16_t port,
                              AvahiStringList* txt) {
    if (event == AVAHI_RESOLVER_FOUND && address != nullptr && address->proto == AVAHI_PROTO_INET) {
        // The entry is "id=<value>"; the key matches case-insensitively.
        AvahiStringList* entry = avahi_string_list_find(txt, "id");
        std::size_t size = entry ? avahi_string_list_get_size(entry) : 0;
        const auto* text = entry ? avahi_string_list_get_text(entry) : nullptr;
        if (size > 3 && size - 3 <= 64 && text[2] == '=') {
            asio::ip::address_v4 v4(ntohl(address->data.ipv4.address));
            LanSighting sighting{op->instance,
                                 std::string(reinterpret_cast<const char*>(text) + 3, size - 3),
                                 asio::ip::udp::endpoint(v4, port)};
            if (on_sighting_) on_sighting_(sighting);
        }
    }
    finish(op);
}

void LanDiscovery::finish(Op* op) {
    // Freeing a resolver inside its own callback is allowed by avahi-client.
    if (op->resolver) avahi_service_resolver_free(op->resolver);
    op->resolver = nullptr;
    ops_.remove_if([op](const auto& o) { return o.get() == op; });
}

void LanDiscovery::expire_ops() {
    auto now = std::chrono::steady_clock::now();
    ops_.remove_if([now](const auto& o) {
        if (now - o->started < kOpTimeout) return false;
        if (o->resolver) avahi_service_resolver_free(o->resolver);
        return true;
    });
}

}  // namespace ghostdrive::discovery
