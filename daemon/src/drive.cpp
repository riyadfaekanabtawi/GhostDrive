#include "ghostdrive/daemon/drive.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <sstream>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/file.hpp"

namespace ghostdrive::daemon {

using core::Errc;
using core::Error;
using core::Status;

namespace {

constexpr std::array<char, 4> kDriveMagic{'G', 'D', 'D', '1'};
#ifdef _WIN32
constexpr const char* kPrivateDirRule = " must be a directory owned by you that only you can access";
#else
constexpr const char* kPrivateDirRule = " must be a directory owned by you with mode 0700";
#endif

using core::write_private_file;

core::Result<core::Bytes> read_private_file(const std::filesystem::path& path) {
    auto data = core::read_private_file(path);
    if (!data && data.error().code == Errc::not_found)
        return Error{Errc::not_found, "no drive at " + path.parent_path().string()};
    return data;
}

Status save_drive_file(const DrivePaths& paths, const DriveId& id, const core::DriveSecret& secret,
                       const std::string& name) {
    core::Bytes data;
    for (char c : kDriveMagic) data.push_back(static_cast<std::byte>(c));
    data.insert(data.end(), id.begin(), id.end());
    data.insert(data.end(), secret.begin(), secret.end());
    auto n = std::min<std::size_t>(name.size(), kMaxNameBytes);
    data.push_back(static_cast<std::byte>(n));
    auto name_bytes = core::as_bytes(std::string_view(name).substr(0, n));
    data.insert(data.end(), name_bytes.begin(), name_bytes.end());
    auto st = write_private_file(paths.drive, data);
    core::secure_zero(data);
    return st;
}

core::Result<std::unique_ptr<Drive>> assemble(const DrivePaths& paths, core::DeviceKey key,
                                              const DriveId& id, const core::DriveSecret& secret,
                                              std::string name) {
    std::error_code ec;
    std::filesystem::create_directories(paths.staging, ec);
    auto owned_key = std::make_unique<core::DeviceKey>(std::move(key));
    auto index = core::MetadataIndex::open(paths.index, id, *owned_key);
    if (!index) return index.error();
    auto store = core::ChunkStore::open(paths.chunks, core::ChunkHasher(secret));
    if (!store) return store.error();
    Membership membership(id, owned_key->public_key());
    DiscoveryKeys discovery_keys(id, owned_key->public_key(), secret);
    return std::make_unique<Drive>(paths, std::move(owned_key), id, secret, std::move(name),
                                   std::move(index).value(), std::move(store).value(),
                                   std::move(membership), std::move(discovery_keys));
}

// Unix socket paths are limited to about 104 bytes (sun_path). A data directory deep enough to
// exceed that gets its socket in the per-user temporary directory instead, named by a hash of
// the data directory so each drive still has its own.
std::filesystem::path control_socket_for(const std::filesystem::path& root) {
    constexpr std::size_t kMaxSocketPath = 100;
    auto preferred = root / "control.sock";
    if (preferred.string().size() < kMaxSocketPath) return preferred;
    // Only a per-user private directory will do (macOS TMPDIR, Linux XDG_RUNTIME_DIR, the
    // Windows user temporary directory if private). A shared
    // one such as /tmp would let another user squat the name and answer `invite` with their
    // own drive. With neither, keep the preferred path and let binding report it as too long.
#ifdef _WIN32
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec || !core::is_private_dir(base)) return preferred;
#else
#ifdef __APPLE__
    const char* dir = std::getenv("TMPDIR");
#else
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
#endif
    if (dir == nullptr || *dir == '\0' || !core::is_private_dir(dir)) return preferred;
    std::filesystem::path base = dir;
#endif
    auto hash = std::hash<std::string>{}(std::filesystem::absolute(root).string());
    char name[64];
    std::snprintf(name, sizeof(name), "ghostdrive-%016zx.sock", hash);
    return base / name;
}

}  // namespace

DrivePaths::DrivePaths(std::filesystem::path r)
    : root(std::move(r)),
      key(root / "device.key"),
      drive(root / "drive"),
      index(root / "index.db"),
      chunks(root / "chunks"),
      staging(root / "staging"),
      peers(root / "peers"),
      control(control_socket_for(root)),
      rendezvous(root / "rendezvous"),
      membership(root / "membership"),
      discovery(root / "discovery") {}

std::filesystem::path default_data_dir() {
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
        return std::filesystem::path(local) / "GhostDrive";
    }
#endif
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home ? home : ".";
#ifdef __APPLE__
    return base / "Library" / "Application Support" / "GhostDrive";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "ghostdrive";
    }
    return base / ".local" / "share" / "ghostdrive";
#endif
}

std::filesystem::path default_mountpoint() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return std::filesystem::path(home ? home : ".") / "GhostDrive";
}

Drive::Drive(DrivePaths paths, std::unique_ptr<core::DeviceKey> key, DriveId drive_id,
             core::DriveSecret secret, std::string device_name, core::MetadataIndex index,
             core::ChunkStore store, Membership membership, DiscoveryKeys discovery_keys)
    : paths_(std::move(paths)),
      key_(std::move(key)),
      drive_id_(drive_id),
      secret_(secret),
      // Shown in terminals and sent to peers; a name stored before this was cleaned is too.
      device_name_(printable_name(device_name)),
      index_(std::move(index)),
      store_(std::move(store)),
      membership_(std::move(membership)),
      discovery_keys_(std::move(discovery_keys)) {}

Drive::~Drive() {
    core::secure_zero(secret_);
}

bool Drive::exists(const std::filesystem::path& root) {
    std::error_code ec;
    return std::filesystem::exists(DrivePaths(root).drive, ec);
}

core::Result<core::DeviceKey> Drive::load_or_create_key(const std::filesystem::path& root) {
    DrivePaths paths(root);
    std::error_code ec;
    std::filesystem::create_directories(paths.root, ec);
    if (!core::make_private_dir(paths.root) || !core::is_private_dir(paths.root)) {
        return Error{Errc::permission, paths.root.string() + kPrivateDirRule};
    }
    auto loaded = core::DeviceKey::load(paths.key);
    if (loaded) return loaded;
    if (loaded.error().code != Errc::not_found) return loaded.error();
    auto key = core::DeviceKey::generate();
    if (!key) return key.error();
    if (auto st = key.value().save(paths.key); !st) return st.error();
    return key;
}

core::Result<std::unique_ptr<Drive>> Drive::create(const std::filesystem::path& root,
                                                   const std::string& device_name) {
    DrivePaths paths(root);
    if (exists(root))
        return Error{Errc::already_exists, "a drive already exists at " + root.string()};
    auto key = load_or_create_key(root);
    if (!key) return key.error();
    DriveId id{};
    if (auto st = core::random_bytes(id); !st) return st.error();
    auto secret = core::generate_drive_secret();
    if (!secret) return secret.error();
    if (auto st = save_drive_file(paths, id, secret.value(), device_name); !st) return st.error();
    auto drive = assemble(paths, std::move(key).value(), id, secret.value(), device_name);
    if (!drive) return drive.error();
    if (auto st = drive.value()->save_membership(); !st) return st.error();
    if (auto st = drive.value()->save_discovery_keys(); !st) return st.error();
    return drive;
}

core::Result<std::unique_ptr<Drive>> Drive::join(const std::filesystem::path& root,
                                                 const std::string& device_name,
                                                 const DriveInfo& info,
                                                 const core::PublicKey& inviter) {
    DrivePaths paths(root);
    if (exists(root))
        return Error{Errc::already_exists, "a drive already exists at " + root.string()};
    auto key = load_or_create_key(root);
    if (!key) return key.error();
    if (auto st = save_drive_file(paths, info.drive_id, info.secret, device_name); !st) {
        return st.error();
    }
    auto drive = assemble(paths, std::move(key).value(), info.drive_id, info.secret, device_name);
    if (!drive) return drive.error();
    if (auto config = RendezvousConfig::parse(info.rendezvous)) {
        (void)drive.value()->set_rendezvous(*config);
    }
    // The invite code pinned the inviter, so this device vouches for it; the inviter's entries
    // then vouch for everyone else.
    auto& d = *drive.value();
    std::string inviter_name = "device";
    for (const auto& member : info.members) {
        if (member.key == inviter) inviter_name = member.name;
    }
    if (auto st = d.membership().add(d.key(), inviter, inviter_name, false); !st) return st.error();
    // It vouches for the others itself only once the daemon has seen them as members for a
    // while (ServiceOptions::countersign_after).
    d.membership().merge(info.membership);
    if (auto st = d.save_membership(); !st) return st.error();
    if (auto applied = d.apply_membership(); !applied) return applied.error();
    // The inviter's keys already account for the devices removed so far.
    auto view = d.membership().view();
    // Its retired signers too, which the membership entries may no longer name. On the
    // inviter's word only, so provisionally: they let adopted records be checked, and a device
    // this one sees as a member, now or later, is not held to them.
    for (const auto& retired : info.retired_signers) {
        if (view.members.contains(retired) || retired == d.key().public_key()) continue;
        if (auto st = d.index().retire_signer(retired, true); !st) return st.error();
    }
    std::set<core::PublicKey> members;
    for (const auto& [k, name] : view.members) members.insert(k);
    d.discovery_keys().merge(info.discovery_keys, members);
    d.discovery_keys().set_removed(view.removed);
    if (auto st = d.save_discovery_keys(); !st) return st.error();
    for (const auto& member : info.members) {
        auto pinned = d.index().is_pinned(member.key);
        if (!pinned || !pinned.value()) continue;
        // A bad address from the inviter is not fatal; the member can still dial us.
        if (!member.address.empty()) (void)d.set_peer_address(member.key, member.address);
    }
    return drive;
}

core::Result<std::unique_ptr<Drive>> Drive::open(const std::filesystem::path& root) {
    DrivePaths paths(root);
    // The index, chunks and staging files rely on the directory's mode for their privacy.
    if (std::filesystem::exists(paths.root) && !core::is_private_dir(paths.root)) {
        return Error{Errc::permission, paths.root.string() + kPrivateDirRule};
    }
    auto data = read_private_file(paths.drive);
    if (!data) return data.error();
    const auto& d = data.value();
    constexpr std::size_t kFixed = kDriveMagic.size() + kDriveIdSize + core::kDriveSecretSize + 1;
    if (d.size() < kFixed || std::memcmp(d.data(), kDriveMagic.data(), kDriveMagic.size()) != 0) {
        return Error{Errc::corrupt, paths.drive.string() + " is not a GhostDrive drive file"};
    }
    DriveId id{};
    core::DriveSecret secret{};
    std::size_t pos = kDriveMagic.size();
    std::copy_n(d.begin() + static_cast<std::ptrdiff_t>(pos), id.size(), id.begin());
    pos += id.size();
    std::copy_n(d.begin() + static_cast<std::ptrdiff_t>(pos), secret.size(), secret.begin());
    pos += secret.size();
    auto name_len = std::to_integer<std::size_t>(d[pos++]);
    if (d.size() != pos + name_len) return Error{Errc::corrupt, "drive file has the wrong size"};
    std::string name(reinterpret_cast<const char*>(d.data() + pos), name_len);
    core::secure_zero(data.value());

    auto key = core::DeviceKey::load(paths.key);
    if (!key) return key.error();
    auto drive = assemble(paths, std::move(key).value(), id, secret, std::move(name));
    core::secure_zero(secret);
    if (!drive) return drive.error();
    auto& opened = *drive.value();
    auto stored = read_private_file(paths.membership);
    if (stored) {
        auto parsed = Membership::parse(stored.value(), id, opened.key().public_key());
        if (!parsed) return parsed.error();
        opened.membership_ = std::move(parsed).value();
    } else if (stored.error().code == Errc::not_found) {
        // A drive from before membership entries: vouch for the devices pinned so far.
        auto devices = opened.index().devices();
        if (!devices) return devices.error();
        for (const auto& device : devices.value()) {
            if (auto st = opened.membership_.add(opened.key(), device.key, device.name, false); !st) {
                return st.error();
            }
        }
        if (auto st = opened.save_membership(); !st) return st.error();
    } else {
        return stored.error();
    }
    // A drive from before discovery keys has none; the daemon makes one if devices were removed.
    auto keys = read_private_file(paths.discovery);
    if (keys) {
        auto parsed =
            DiscoveryKeys::parse(keys.value(), id, opened.key().public_key(), opened.secret());
        core::secure_zero(keys.value());
        if (!parsed) return parsed.error();
        opened.discovery_keys_ = std::move(parsed).value();
    } else if (keys.error().code != Errc::not_found) {
        return keys.error();
    }
    if (auto st = opened.retire_removed(nullptr); !st) return st.error();
    return drive;
}

Status Drive::save_membership() {
    return write_private_file(paths_.membership, membership_.serialize());
}

Status Drive::save_discovery_keys() {
    auto data = discovery_keys_.serialize();
    auto st = write_private_file(paths_.discovery, data);
    core::secure_zero(data);
    return st;
}

core::Result<MembershipChange> Drive::apply_membership() {
    auto view = membership_.view();
    auto devices = index_.devices();
    if (!devices) return devices.error();
    MembershipChange change;
    // Before unpinning, so a crash in between never leaves a removed key accepted.
    if (auto st = retire_removed(&change.retired); !st) return st.error();
    for (const auto& device : devices.value()) {
        if (view.members.contains(device.key)) continue;
        if (auto st = index_.remove_device(device.key); !st) return st.error();
        change.unpinned.push_back(device.key);
    }
    for (const auto& [key, name] : view.members) {
        auto pinned = index_.is_pinned(key);
        if (!pinned) return pinned.error();
        if (pinned.value()) continue;
        auto st = index_.add_device(key, name);
        // Another key with the same 64-bit short ID cannot be told apart in version vectors.
        if (!st && st.error().code != Errc::already_exists) return st.error();
        if (st) change.pinned.push_back(key);
    }
    return change;
}

Status Drive::retire_removed(std::vector<core::PublicKey>* newly) {
    // Only devices removed for good: one merely in doubt, or whose removal may still be
    // undone, must not have its records rewritten.
    auto view = membership_.view();
    for (const auto& key : view.removed) {
        if (view.removal_pending.contains(key)) continue;
        const auto id = core::short_id(key);
        if (index_.is_confirmed_retired(id)) continue;
        if (auto adopted = index_.retire_signer(key); !adopted) return adopted.error();
        if (newly && index_.is_confirmed_retired(id)) newly->push_back(key);
    }
    return {};
}

std::map<core::PublicKey, std::string> Drive::peer_addresses() const {
    std::lock_guard lock(peers_mutex_);
    std::map<core::PublicKey, std::string> out;
    std::ifstream in(paths_.peers);
    std::string hex;
    std::string address;
    while (in >> hex >> address) {
        auto bytes = core::from_hex(hex);
        if (!bytes || bytes->size() != core::kPublicKeySize) continue;
        core::PublicKey key{};
        std::copy(bytes->begin(), bytes->end(), key.begin());
        out[key] = address;
    }
    return out;
}

Status Drive::set_peer_address(const core::PublicKey& key, const std::string& address) {
    // The peers file is whitespace separated, and addresses can come from other devices.
    if (address.empty() || address.size() > 64 ||
        std::any_of(address.begin(), address.end(), [](char c) {
            return !(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' ||
                     c == '[' || c == ']' || c == '-');
        })) {
        return Error{Errc::invalid_argument, "invalid peer address"};
    }
    auto all = peer_addresses();
    all[key] = address;
    std::lock_guard lock(peers_mutex_);
    std::ostringstream out;
    for (const auto& [k, a] : all) out << core::to_hex(k) << ' ' << a << '\n';
    auto text = out.str();
    return write_private_file(paths_.peers, core::as_bytes(text));
}

std::string RendezvousConfig::to_string() const {
    return host + ":" + std::to_string(port) + " " + core::fingerprint(key);
}

std::optional<RendezvousConfig> RendezvousConfig::parse(std::string_view text) {
    auto space = text.find(' ');
    if (space == std::string_view::npos) return std::nullopt;
    auto address = text.substr(0, space);
    auto key = core::parse_fingerprint(text.substr(space + 1));
    auto colon = address.rfind(':');
    if (!key || colon == std::string_view::npos || colon == 0) return std::nullopt;
    RendezvousConfig config;
    config.host = std::string(address.substr(0, colon));
    auto digits = address.substr(colon + 1);
    auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), config.port);
    if (err != std::errc{} || end != digits.data() + digits.size() || config.port == 0) {
        return std::nullopt;
    }
    // Host names and numeric addresses only; the value is written to a file and shown to users.
    if (config.host.size() > 253 ||
        std::any_of(config.host.begin(), config.host.end(), [](char c) {
            return !(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
                     c == ':' || c == '[' || c == ']');
        })) {
        return std::nullopt;
    }
    config.key = *key;
    return config;
}

std::optional<RendezvousConfig> Drive::rendezvous() const {
    std::ifstream in(paths_.rendezvous);
    std::string line;
    if (!std::getline(in, line)) return std::nullopt;
    return RendezvousConfig::parse(line);
}

Status Drive::set_rendezvous(const RendezvousConfig& config) {
    auto text = config.to_string() + "\n";
    return write_private_file(paths_.rendezvous, core::as_bytes(text));
}

}  // namespace ghostdrive::daemon
