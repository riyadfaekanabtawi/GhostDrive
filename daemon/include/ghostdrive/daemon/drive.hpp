#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "ghostdrive/core/chunk_store.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/core/metadata_index.hpp"
#include "ghostdrive/core/result.hpp"
#include "ghostdrive/daemon/codec.hpp"
#include "ghostdrive/daemon/membership.hpp"

namespace ghostdrive::daemon {

// Files under a device's data directory.
struct DrivePaths {
    explicit DrivePaths(std::filesystem::path root);

    std::filesystem::path root;
    std::filesystem::path key;      // Ed25519 device key, 0600
    std::filesystem::path drive;    // drive ID, drive secret, device name, 0600
    std::filesystem::path index;    // SQLite metadata index
    std::filesystem::path chunks;   // content-addressed chunk store
    std::filesystem::path staging;  // staging files of open writers
    std::filesystem::path peers;    // last known address of each member device
    std::filesystem::path control;  // Unix socket of the running daemon
    std::filesystem::path rendezvous;  // "host:port FINGERPRINT" of the rendezvous server
    std::filesystem::path membership;  // signed membership entries, 0600
};

// Default data directory: ~/Library/Application Support/GhostDrive on macOS,
// $XDG_DATA_HOME/ghostdrive (or ~/.local/share/ghostdrive) elsewhere.
std::filesystem::path default_data_dir();
// Default mountpoint: ~/GhostDrive.
std::filesystem::path default_mountpoint();

struct RendezvousConfig {
    std::string host;
    std::uint16_t port = 0;
    core::PublicKey key{};

    std::string to_string() const;  // "host:port FINGERPRINT"
    static std::optional<RendezvousConfig> parse(std::string_view text);
};

// One device's membership of one drive: identity, secret, index, chunks and peer addresses.
class Drive {
public:
    // A new drive with this device as its only member.
    static core::Result<std::unique_ptr<Drive>> create(const std::filesystem::path& root,
                                                       const std::string& device_name);
    // Joins the drive described by `info`, received from `inviter` during pairing.
    static core::Result<std::unique_ptr<Drive>> join(const std::filesystem::path& root,
                                                     const std::string& device_name,
                                                     const DriveInfo& info,
                                                     const core::PublicKey& inviter);
    static core::Result<std::unique_ptr<Drive>> open(const std::filesystem::path& root);
    // The device key, created on first use. Joining needs it before the drive exists.
    static core::Result<core::DeviceKey> load_or_create_key(const std::filesystem::path& root);
    static bool exists(const std::filesystem::path& root);

    const DrivePaths& paths() const noexcept { return paths_; }
    const core::DeviceKey& key() const noexcept { return key_; }
    const DriveId& drive_id() const noexcept { return drive_id_; }
    const core::DriveSecret& secret() const noexcept { return secret_; }
    const std::string& device_name() const noexcept { return device_name_; }
    core::MetadataIndex& index() noexcept { return index_; }
    core::ChunkStore& store() noexcept { return store_; }
    core::HybridClock& clock() noexcept { return clock_; }

    // Not thread-safe; the daemon uses it from its I/O executor only.
    Membership& membership() noexcept { return membership_; }
    core::Status save_membership();
    // Brings the pinned devices in the index in line with the membership view. Returns the
    // devices that were unpinned.
    core::Result<std::vector<core::PublicKey>> apply_membership();

    // Thread-safe.
    std::map<core::PublicKey, std::string> peer_addresses() const;
    core::Status set_peer_address(const core::PublicKey& key, const std::string& address);

    // The rendezvous server, if one is configured. Re-read on every call, so the daemon
    // notices a change made with `ghostdrive rendezvous` while it runs.
    std::optional<RendezvousConfig> rendezvous() const;
    core::Status set_rendezvous(const RendezvousConfig& config);

    Drive(DrivePaths paths, core::DeviceKey key, DriveId drive_id, core::DriveSecret secret,
          std::string device_name, core::MetadataIndex index, core::ChunkStore store,
          Membership membership);
    ~Drive();

private:
    DrivePaths paths_;
    core::DeviceKey key_;
    DriveId drive_id_;
    core::DriveSecret secret_;
    std::string device_name_;
    core::MetadataIndex index_;
    core::ChunkStore store_;
    core::HybridClock clock_;
    Membership membership_;
    mutable std::mutex peers_mutex_;
};

}  // namespace ghostdrive::daemon
