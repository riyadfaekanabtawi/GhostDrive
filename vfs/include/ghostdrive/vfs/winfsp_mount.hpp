#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "ghostdrive/core/result.hpp"
#include "ghostdrive/vfs/filesystem.hpp"

namespace ghostdrive::vfs {

struct WinFspMountOptions {
    std::string volume_name = "GhostDrive";
    // How long Windows may cache file information, security and directory listings. Data is never
    // cached by Windows; every read reaches the Filesystem and its chunk cache.
    std::chrono::milliseconds info_timeout{1000};
    // Free and total space reported for the volume: those of the disk holding this path (the
    // data directory). Empty reports zero.
    std::filesystem::path capacity_from;
    bool debug = false;
};

// A WinFsp mount of a Filesystem (native FSP_FILE_SYSTEM_INTERFACE), read-only unless the
// Filesystem has WriteSupport. The mountpoint is a drive letter ("X:") or a directory that does
// not exist yet (an empty one is removed first); WinFsp creates it and removes it on unmount.
// Only the current user can open anything in it. mount() fails with Errc::not_found when WinFsp
// is not installed.
class WinFspMount {
public:
    static core::Result<std::unique_ptr<WinFspMount>> mount(Filesystem& fs,
                                                            const std::filesystem::path& mountpoint,
                                                            const WinFspMountOptions& options = {});
    WinFspMount(const WinFspMount&) = delete;
    WinFspMount& operator=(const WinFspMount&) = delete;
    // Unmounts if still mounted.
    ~WinFspMount();

    // Unmounts and waits for operations still completing. Safe to call more than once.
    void unmount();
    const std::filesystem::path& mountpoint() const noexcept;

    struct Impl;

private:
    struct Private {};

public:
    WinFspMount(Private, std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ghostdrive::vfs
