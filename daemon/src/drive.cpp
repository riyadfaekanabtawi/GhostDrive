#include "ghostdrive/daemon/drive.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <sstream>

#include "ghostdrive/core/bytes.hpp"

namespace ghostdrive::daemon {

using core::Errc;
using core::Error;
using core::Status;

namespace {

constexpr std::array<char, 4> kDriveMagic{'G', 'D', 'D', '1'};

Error errno_error(const std::string& what, const std::filesystem::path& path) {
    return Error{Errc::io, what + " " + path.string() + ": " + std::strerror(errno)};
}

// Writes `data` to `path` via a 0600 temp file and rename, so a crash never leaves a partial
// file and the secret is never readable by others, even briefly.
Status write_private_file(const std::filesystem::path& path, std::span<const std::byte> data) {
    auto tmp = path;
    tmp += ".tmp";
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return errno_error("open", tmp);
    const std::byte* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            auto err = errno_error("write", tmp);
            ::close(fd);
            return err;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        auto err = errno_error("fsync", tmp);
        ::close(fd);
        ::unlink(tmp.c_str());
        return err;
    }
    if (::close(fd) != 0 || ::rename(tmp.c_str(), path.c_str()) != 0) {
        auto err = errno_error("rename", tmp);
        ::unlink(tmp.c_str());  // never leave a stray copy of the secret behind
        return err;
    }
    return {};
}

core::Result<core::Bytes> read_private_file(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return Error{Errc::not_found, "no drive at " + path.parent_path().string()};
        return errno_error("open", path);
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return errno_error("fstat", path);
    }
    if ((st.st_mode & 077) != 0 || st.st_uid != ::geteuid() || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return Error{Errc::permission,
                     path.string() + " must be a regular file owned by you with mode 0600"};
    }
    core::Bytes data(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < data.size()) {
        ssize_t n = ::read(fd, data.data() + got, data.size() - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (got != data.size()) return Error{Errc::io, "short read of " + path.string()};
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
    auto index = core::MetadataIndex::open(paths.index, core::short_id(key.public_key()));
    if (!index) return index.error();
    auto store = core::ChunkStore::open(paths.chunks, core::ChunkHasher(secret));
    if (!store) return store.error();
    return std::make_unique<Drive>(paths, std::move(key), id, secret, std::move(name),
                                   std::move(index).value(), std::move(store).value());
}

// Unix socket paths are limited to about 104 bytes (sun_path). A data directory deep enough to
// exceed that gets its socket in the per-user temporary directory instead, named by a hash of
// the data directory so each drive still has its own.
// A directory only this user can enter: owned by us, no group or other access.
bool private_dir(const std::filesystem::path& dir) {
    struct stat st{};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::geteuid() &&
           (st.st_mode & 077) == 0;
}

std::filesystem::path control_socket_for(const std::filesystem::path& root) {
    constexpr std::size_t kMaxSocketPath = 100;
    auto preferred = root / "control.sock";
    if (preferred.string().size() < kMaxSocketPath) return preferred;
    // Only a per-user private directory will do (macOS TMPDIR, Linux XDG_RUNTIME_DIR). A shared
    // one such as /tmp would let another user squat the name and answer `invite` with their
    // own drive. With neither, keep the preferred path and let binding report it as too long.
#ifdef __APPLE__
    const char* dir = std::getenv("TMPDIR");
#else
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
#endif
    if (dir == nullptr || *dir == '\0' || !private_dir(dir)) return preferred;
    std::filesystem::path base = dir;
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
      rendezvous(root / "rendezvous") {}

std::filesystem::path default_data_dir() {
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
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / "GhostDrive";
}

Drive::Drive(DrivePaths paths, core::DeviceKey key, DriveId drive_id, core::DriveSecret secret,
             std::string device_name, core::MetadataIndex index, core::ChunkStore store)
    : paths_(std::move(paths)),
      key_(std::move(key)),
      drive_id_(drive_id),
      secret_(secret),
      device_name_(std::move(device_name)),
      index_(std::move(index)),
      store_(std::move(store)) {}

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
    if (::chmod(paths.root.c_str(), 0700) != 0 || !private_dir(paths.root)) {
        return Error{Errc::permission,
                     paths.root.string() + " must be a directory owned by you with mode 0700"};
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
    return assemble(paths, std::move(key).value(), id, secret.value(), device_name);
}

core::Result<std::unique_ptr<Drive>> Drive::join(const std::filesystem::path& root,
                                                 const std::string& device_name,
                                                 const DriveInfo& info) {
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
    for (const auto& member : info.members) {
        if (member.key == drive.value()->key().public_key()) continue;
        if (auto st = drive.value()->index().add_device(member.key, member.name); !st) {
            return st.error();
        }
        // A bad address from the inviter is not fatal; the member can still dial us.
        if (!member.address.empty()) (void)drive.value()->set_peer_address(member.key, member.address);
    }
    return drive;
}

core::Result<std::unique_ptr<Drive>> Drive::open(const std::filesystem::path& root) {
    DrivePaths paths(root);
    // The index, chunks and staging files rely on the directory's mode for their privacy.
    if (std::filesystem::exists(paths.root) && !private_dir(paths.root)) {
        return Error{Errc::permission,
                     paths.root.string() + " must be a directory owned by you with mode 0700"};
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
    return drive;
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
