// LanDiscovery through the DNS-SD API of windns.h (DnsServiceRegister, DnsServiceBrowse,
// DnsServiceResolve; Windows 10 1809 and later), answered by the system's mDNS responder in the
// DNS Client service. The functions are looked up in dnsapi.dll at run time, so on an older
// Windows start() fails and the daemon carries on without LAN discovery.
//
// Threading and lifetime. dnsapi calls back on its own thread pool, and the cancel functions
// call back synchronously on the calling thread (seen on Windows Server 2025). Each request is a
// Call owned by a process-wide registry and named by a token; the token, not a pointer, is the
// request's context. A callback copies what it needs out of the records (which it owns and
// frees at once), looks its token up under the registry lock and, while still holding it, posts
// the result to the owner's executor. ~LanDiscovery detaches its Calls under the same lock before
// cancelling them, so no callback posts to an executor after the owner is gone, and a Call stays
// in the registry, keeping its request structures alive, until its final callback: the cancel
// callback for a browse, the one result for a resolve, the deregistration for a registration.
// The registry lock is never held while calling into dnsapi.
#include "ghostdrive/discovery/lan_discovery.hpp"
// After the Asio headers in lan_discovery.hpp, which include winsock2.h and windows.h; windns.h
// needs their types.
#include <windns.h>

#include <algorithm>
#include <array>
#include <asio/post.hpp>
#include <bit>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

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
constexpr std::size_t kMaxKnown = 256;     // advertisements remembered, so each resolves once
constexpr std::size_t kMaxPerBrowse = 64;  // names taken from one browse callback
constexpr std::size_t kMaxIdLength = 64;
constexpr const wchar_t* kServiceDomain = L"_ghostdrive._udp.local";

// The DNS-SD functions, from dnsapi.dll. decltype keeps the signatures those of the SDK header.
struct Api {
    decltype(&DnsServiceConstructInstance) construct_instance = nullptr;
    decltype(&DnsServiceFreeInstance) free_instance = nullptr;
    decltype(&DnsServiceRegister) register_service = nullptr;
    decltype(&DnsServiceDeRegister) deregister_service = nullptr;
    decltype(&DnsServiceRegisterCancel) register_cancel = nullptr;
    decltype(&DnsServiceBrowse) browse = nullptr;
    decltype(&DnsServiceBrowseCancel) browse_cancel = nullptr;
    decltype(&DnsServiceResolve) resolve = nullptr;
    decltype(&DnsServiceResolveCancel) resolve_cancel = nullptr;
    bool ok = false;
};

template <typename F>
bool load(HMODULE module, const char* name, F& out) {
    FARPROC proc = ::GetProcAddress(module, name);
    if (proc == nullptr) return false;
    out = std::bit_cast<F>(proc);
    return true;
}

const Api& api() {
    static const Api loaded = [] {
        Api a;
        // dnsapi.dll stays loaded for the life of the process (the import of DnsFree holds it).
        HMODULE module = ::LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) return a;
        a.ok = load(module, "DnsServiceConstructInstance", a.construct_instance) &&
               load(module, "DnsServiceFreeInstance", a.free_instance) &&
               load(module, "DnsServiceRegister", a.register_service) &&
               load(module, "DnsServiceDeRegister", a.deregister_service) &&
               load(module, "DnsServiceRegisterCancel", a.register_cancel) &&
               load(module, "DnsServiceBrowse", a.browse) &&
               load(module, "DnsServiceBrowseCancel", a.browse_cancel) &&
               load(module, "DnsServiceResolve", a.resolve) &&
               load(module, "DnsServiceResolveCancel", a.resolve_cancel);
        return a;
    }();
    return loaded;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                  static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          out.data(), n);
    return out;
}

// Empty for null, empty or invalid UTF-16.
std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return {};
    int n = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr,
                                  nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out.data(), n, nullptr, nullptr);
    out.pop_back();  // the terminator
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto x = static_cast<unsigned char>(a[i]);
        auto y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// "<instance>._ghostdrive._udp.local" to "<instance>", or empty if it is another service.
std::string instance_of(const std::string& full) {
    const std::string suffix = std::string(".") + kServiceType + ".local";
    if (full.size() <= suffix.size()) return {};
    auto cut = full.size() - suffix.size();
    if (!iequals(std::string_view(full).substr(cut), suffix)) return {};
    return full.substr(0, cut);
}

// What one resolve found, copied out of the DNS_SERVICE_INSTANCE.
struct Resolved {
    bool ok = false;
    std::string id;
    std::uint16_t port = 0;
    std::optional<asio::ip::address_v4> address;
};

// One request handed to dnsapi. The request structures, the strings they point to, the cancel
// handle and the registered instance live here until the request's final callback.
struct Call {
    enum class Kind { browse, resolve, registration };
    Kind kind;
    std::uint64_t token = 0;

    // Where results go; cleared (under the registry lock) when the owner goes away.
    bool attached = true;
    asio::any_io_executor executor;
    LanDiscovery* owner = nullptr;
    std::shared_ptr<bool> alive;

    std::wstring query;  // QueryName of a browse or resolve
    DNS_SERVICE_CANCEL cancel{};
    DNS_SERVICE_BROWSE_REQUEST browse{};
    DNS_SERVICE_RESOLVE_REQUEST resolve{};
    DNS_SERVICE_REGISTER_REQUEST registration{};
    PDNS_SERVICE_INSTANCE instance = nullptr;  // what we registered
    bool registered = false;                   // the registration callback reported success
    bool deregistering = false;

    Call(Kind k, asio::any_io_executor ex, LanDiscovery* o, std::shared_ptr<bool> a)
        : kind(k), executor(std::move(ex)), owner(o), alive(std::move(a)) {}
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    ~Call() {
        if (instance != nullptr) api().free_instance(instance);
    }

    void* context() const { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(token)); }

    // Runs `fn(owner)` on the owner's executor if the owner still exists. Registry lock held.
    template <typename Fn>
    void post(Fn fn) {
        if (!attached) return;
        asio::post(executor, [o = owner, a = alive, f = std::move(fn)]() mutable {
            if (*a) f(*o);
        });
    }
};

// Process-wide, so a callback that arrives after its owner is gone still finds valid memory.
// Destroyed at exit after every LanDiscovery is gone; dnsapi is done with all requests by then.
struct Registry {
    std::mutex mutex;
    std::uint64_t next = 1;
    std::map<std::uint64_t, std::shared_ptr<Call>> calls;
};

Registry& registry() {
    static Registry r;
    return r;
}

std::shared_ptr<Call> add_call(Call::Kind kind, asio::any_io_executor executor, LanDiscovery* owner,
                               std::shared_ptr<bool> alive) {
    auto call = std::make_shared<Call>(kind, std::move(executor), owner, std::move(alive));
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    call->token = r.next++;
    r.calls.emplace(call->token, call);
    return call;
}

void erase_call(std::uint64_t token) {
    std::shared_ptr<Call> last;  // released after the lock, since ~Call calls into dnsapi
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    if (auto it = r.calls.find(token); it != r.calls.end()) {
        last = std::move(it->second);
        r.calls.erase(it);
    }
}

std::uint64_t token_of(void* context) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(context));
}

VOID WINAPI browse_reply(DWORD status, PVOID context, PDNS_RECORD records) {
    // The records are ours to free. Only the PTR records of our service type matter: each
    // names one advertisement; a TTL of 0 is its goodbye.
    // COPY: dnsapi's records are freed here, before the result runs on the executor.
    std::vector<std::pair<std::string, bool>> names;
    for (auto* r = reinterpret_cast<PDNS_RECORDW>(records); r != nullptr; r = r->pNext) {
        if (r->wType != DNS_TYPE_PTR || r->pName == nullptr || r->Data.PTR.pNameHost == nullptr ||
            ::_wcsicmp(r->pName, kServiceDomain) != 0) {
            continue;
        }
        if (names.size() >= kMaxPerBrowse) break;
        auto name = narrow(r->Data.PTR.pNameHost);
        if (!name.empty()) names.emplace_back(std::move(name), r->dwTtl != 0);
    }
    if (records != nullptr) DnsRecordListFree(records, DnsFreeRecordList);

    auto token = token_of(context);
    if (status == ERROR_CANCELLED) {
        erase_call(token);  // the last callback of a cancelled browse
        return;
    }
    if (status != ERROR_SUCCESS || names.empty()) return;
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto it = r.calls.find(token);
    if (it == r.calls.end()) return;
    it->second->post([names = std::move(names)](LanDiscovery& owner) mutable {
        owner.on_browse(std::move(names));
    });
}

VOID WINAPI resolve_reply(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    // COPY: the instance is ours to free, here, before the result runs on the executor.
    Resolved found;
    if (status == ERROR_SUCCESS && instance != nullptr) {
        for (DWORD i = 0; instance->keys != nullptr && i < instance->dwPropertyCount; ++i) {
            if (instance->keys[i] != nullptr && ::_wcsicmp(instance->keys[i], L"id") == 0 &&
                instance->values != nullptr) {
                found.id = narrow(instance->values[i]);
                break;
            }
        }
        found.port = instance->wPort;
        if (instance->ip4Address != nullptr) {
            // IP4_ADDRESS holds the address in network byte order.
            asio::ip::address_v4::bytes_type bytes{};
            std::memcpy(bytes.data(), instance->ip4Address, bytes.size());
            found.address = asio::ip::address_v4(bytes);
        }
        found.ok = true;
    }
    if (instance != nullptr) api().free_instance(instance);

    // A resolve calls back once: with its result, or with ERROR_CANCELLED once cancelled.
    auto token = token_of(context);
    std::shared_ptr<Call> last;
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto it = r.calls.find(token);
    if (it == r.calls.end()) return;
    last = std::move(it->second);
    r.calls.erase(it);
    last->post([token, found = std::move(found)](LanDiscovery& owner) mutable {
        owner.on_resolve(token, found.ok, std::move(found.id), found.port, found.address);
    });
}

VOID WINAPI register_reply(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    // The registration's own callback and its deregistration's both arrive here, with a copy of
    // the instance that is ours to free.
    if (instance != nullptr) api().free_instance(instance);
    auto token = token_of(context);
    std::shared_ptr<Call> last;
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto it = r.calls.find(token);
    if (it == r.calls.end()) return;
    Call& call = *it->second;
    LanDiscovery::Registration state = LanDiscovery::Registration::failed;
    if (call.deregistering) {
        state = LanDiscovery::Registration::gone;
    } else if (status == ERROR_SUCCESS) {
        state = LanDiscovery::Registration::registered;
        call.registered = true;
    }
    call.post([token, state](LanDiscovery& owner) { owner.on_register(token, state); });
    if (state != LanDiscovery::Registration::registered) {
        // Deregistered, cancelled or refused: nothing more comes for this request.
        last = std::move(it->second);
        r.calls.erase(it);
    }
}

std::wstring host_name() {
    std::array<wchar_t, 256> name{};
    DWORD size = static_cast<DWORD>(name.size());
    if (!::GetComputerNameExW(ComputerNameDnsHostname, name.data(), &size) || size == 0) return {};
    return std::wstring(name.data(), size) + L".local";
}

core::Result<std::string> random_instance() {
    // A random instance name, so the name itself identifies nothing.
    std::array<std::byte, 8> random{};
    if (auto st = core::random_bytes(random); !st) return st.error();
    return "ghostdrive-" + core::to_hex(random);
}

std::shared_ptr<Call> find_call(std::uint64_t token) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto it = r.calls.find(token);
    return it == r.calls.end() ? nullptr : it->second;
}

// Stops one request; false if no final callback will come. dnsapi may run the final callback
// inside the cancel call, which erases the Call from the registry; `call` keeps its structures
// alive until the cancel function returns.
bool cancel_call(const std::shared_ptr<Call>& call) {
    const Api& a = api();
    bool pending = true;
    switch (call->kind) {
        case Call::Kind::browse:
            pending = a.browse_cancel(&call->cancel) == ERROR_SUCCESS;
            break;
        case Call::Kind::resolve:
            pending = a.resolve_cancel(&call->cancel) == ERROR_SUCCESS;
            break;
        case Call::Kind::registration: {
            bool registered = false;
            {
                std::lock_guard lock(registry().mutex);
                if (call->deregistering) return true;  // its callback is on the way
                registered = call->registered;
                call->deregistering = true;
            }
            // A deregistration sent before the registration completed fails and leaves the
            // service registered, so a pending registration is cancelled instead.
            if (!registered && a.register_cancel(&call->cancel) == ERROR_SUCCESS) return true;
            pending = a.deregister_service(&call->registration, nullptr) == DNS_REQUEST_PENDING;
            break;
        }
    }
    if (!pending) erase_call(call->token);
    return pending;
}

}  // namespace

struct LanDiscovery::Op {
    std::uint64_t token = 0;
    std::string name;  // the full service instance name
    std::string instance;
    std::chrono::steady_clock::time_point started;
};

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
    if (!api().ok) {
        return Error{Errc::io, "this Windows has no DNS-SD API (Windows 10 1809 or later)"};
    }
    auto name = random_instance();
    if (!name) return name.error();
    instance_ = std::move(name).value();
    advertised_id_ = current_id_();
    host_ = host_name();
    if (host_.empty()) return Error{Errc::io, "cannot read the computer's host name"};

    if (auto st = register_service(); !st) return st;
    auto browse = add_call(Call::Kind::browse, executor_, this, alive_);
    calls_.push_back(browse->token);
    browse->query = kServiceDomain;
    browse->browse.Version = DNS_QUERY_REQUEST_VERSION1;
    browse->browse.InterfaceIndex = 0;  // every interface
    browse->browse.QueryName = browse->query.c_str();
    browse->browse.pBrowseCallback = browse_reply;
    browse->browse.pQueryContext = browse->context();
    if (auto err = api().browse(&browse->browse, &browse->cancel); err != DNS_REQUEST_PENDING) {
        erase_call(browse->token);
        return Error{Errc::io, "mDNS browse failed (" + std::to_string(err) + ")"};
    }
    refresh_id();
    return {};
}

core::Status LanDiscovery::register_service() {
    auto call = add_call(Call::Kind::registration, executor_, this, alive_);
    calls_.push_back(call->token);
    const std::wstring full = widen(instance_) + L"." + kServiceDomain;
    const std::wstring id = widen(advertised_id_);
    PCWSTR keys[] = {L"v", L"id"};
    PCWSTR values[] = {L"1", id.c_str()};
    // The instance gets its own copies of the strings.
    call->instance = api().construct_instance(full.c_str(), host_.c_str(), nullptr, nullptr, port_,
                                              0, 0, 2, keys, values);
    if (call->instance == nullptr) {
        erase_call(call->token);
        return Error{Errc::io, "cannot build the mDNS service instance"};
    }
    call->registration.Version = DNS_QUERY_REQUEST_VERSION1;
    call->registration.InterfaceIndex = 0;  // every interface
    call->registration.pServiceInstance = call->instance;
    call->registration.pRegisterCompletionCallback = register_reply;
    call->registration.pQueryContext = call->context();
    call->registration.hCredentials = nullptr;
    call->registration.unicastEnabled = FALSE;  // mDNS
    if (auto err = api().register_service(&call->registration, &call->cancel);
        err != DNS_REQUEST_PENDING) {
        erase_call(call->token);
        return Error{Errc::io, "mDNS registration failed (" + std::to_string(err) + ")"};
    }
    registration_ = call->token;
    registration_state_ = Registration::pending;
    registered_id_ = advertised_id_;
    return {};
}

LanDiscovery::~LanDiscovery() {
    *alive_ = false;
    refresh_timer_.cancel();
    // Detach first, under the lock, so no callback posts to the executor from here on; then
    // cancel outside it, since dnsapi may call back inside the cancel functions.
    std::vector<std::shared_ptr<Call>> calls;
    {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        for (auto token : calls_) {
            auto it = r.calls.find(token);
            if (it == r.calls.end()) continue;
            it->second->attached = false;
            it->second->executor = {};
            calls.push_back(it->second);
        }
    }
    for (auto& call : calls) cancel_call(call);
}

void LanDiscovery::refresh() {
    auto id = current_id_();
    bool changed = id != advertised_id_;
    advertised_id_ = std::move(id);
    // After a refused registration, try again (this runs every kRefreshEvery).
    if (registration_ == 0) {
        (void)register_service();
        return;
    }
    // There is no call to update a registered TXT record, so the service is deregistered and
    // registered again under the same name once the old registration is gone (on_register). A
    // registration still pending is checked when it completes.
    if (changed && registration_state_ == Registration::registered) leave();
}

void LanDiscovery::leave() {
    registration_state_ = Registration::gone;
    auto call = find_call(registration_);
    if (call && cancel_call(call)) return;  // on_register continues
    std::erase(calls_, registration_);
    registration_ = 0;
    (void)register_service();
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

void LanDiscovery::on_register(std::uint64_t token, Registration state) {
    if (token != registration_) return;
    if (state == Registration::registered) {
        registration_state_ = Registration::registered;
        // The ID may have changed while the registration was on its way.
        if (registered_id_ != advertised_id_) leave();
        return;
    }
    std::erase(calls_, token);
    registration_ = 0;
    registration_state_ = Registration::gone;
    // Deregistered for a refresh: advertise the current ID. Refused: refresh() retries later.
    if (state == Registration::gone) (void)register_service();
}

void LanDiscovery::on_browse(std::vector<std::pair<std::string, bool>> names) {
    for (auto& [name, added] : names) {
        if (!added) {
            known_.erase(name);
            continue;
        }
        auto instance = instance_of(name);
        if (instance.empty() || instance == instance_) continue;  // not ours, or ourselves
        if (known_.contains(name) || known_.size() >= kMaxKnown) continue;
        expire_ops();
        if (ops_.size() >= kMaxOps) return;
        auto now = std::chrono::steady_clock::now();
        if (now - window_start_ > kRateWindow) {
            window_start_ = now;
            window_count_ = 0;
        }
        if (++window_count_ > kMaxResolvesPerWindow) return;

        auto call = add_call(Call::Kind::resolve, executor_, this, alive_);
        call->query = widen(name);
        call->resolve.Version = DNS_QUERY_REQUEST_VERSION1;
        call->resolve.InterfaceIndex = 0;
        call->resolve.QueryName = call->query.data();
        call->resolve.pResolveCompletionCallback = resolve_reply;
        call->resolve.pQueryContext = call->context();
        if (call->query.empty() ||
            api().resolve(&call->resolve, &call->cancel) != DNS_REQUEST_PENDING) {
            erase_call(call->token);
            continue;
        }
        calls_.push_back(call->token);
        auto op = std::make_unique<Op>();
        op->token = call->token;
        op->name = name;
        op->instance = std::move(instance);
        op->started = now;
        ops_.push_back(std::move(op));
        known_.insert(name);
    }
}

void LanDiscovery::on_resolve(std::uint64_t token, bool ok, std::string id, std::uint16_t port,
                              std::optional<asio::ip::address_v4> address) {
    std::erase(calls_, token);
    auto it = std::find_if(ops_.begin(), ops_.end(),
                           [token](const auto& op) { return op->token == token; });
    if (it == ops_.end()) return;  // expired and cancelled
    auto op = std::move(*it);
    ops_.erase(it);
    if (!ok || !address || id.empty() || id.size() > kMaxIdLength) {
        known_.erase(op->name);  // a later announcement may try again
        return;
    }
    LanSighting sighting{op->instance, std::move(id), asio::ip::udp::endpoint(*address, port)};
    if (on_sighting_) on_sighting_(sighting);
}

void LanDiscovery::expire_ops() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = ops_.begin(); it != ops_.end();) {
        if (now - (*it)->started < kOpTimeout) {
            ++it;
            continue;
        }
        known_.erase((*it)->name);
        if (auto call = find_call((*it)->token)) cancel_call(call);
        std::erase(calls_, (*it)->token);
        it = ops_.erase(it);
    }
}

}  // namespace ghostdrive::discovery
