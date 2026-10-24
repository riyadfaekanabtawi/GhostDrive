#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ghostdrive/core/result.hpp"
#include "ghostdrive/vfs/filesystem.hpp"

namespace ghostdrive::vfs {

struct FuseMountOptions {
    std::string volume_name = "GhostDrive";
    double entry_timeout_s = 1.0;
    double attr_timeout_s = 1.0;
    bool debug = false;
    // macOS/FUSE-T: let the kernel's NFS client cache attributes and lookups. Off by default:
    // FUSE-T cannot invalidate those caches, so with caching a change from another device can
    // take up to a minute to show; without it every stat is answered from memory here.
    bool kernel_attr_cache = false;
    // Passed through as "-o <value>", for example "rwsize=65536" on FUSE-T.
    std::vector<std::string> extra_options;
};

// A libfuse3 low-level mount of a Filesystem (Linux libfuse3, macOS FUSE-T),
// read-only unless the Filesystem has WriteSupport. The
// session loop runs on its own threads; reads are answered from the Filesystem's executor.
// mount() returns once the mount is visible, and sets SIGPIPE to ignored for the process.
class FuseMount {
public:
    static core::Result<std::unique_ptr<FuseMount>> mount(Filesystem& fs,
                                                          const std::filesystem::path& mountpoint,
                                                          const FuseMountOptions& options = {});
    FuseMount(const FuseMount&) = delete;
    FuseMount& operator=(const FuseMount&) = delete;
    // Unmounts if still mounted.
    ~FuseMount();

    // Unmounts and waits for the session loop to finish. Safe to call more than once.
    void unmount();
    const std::filesystem::path& mountpoint() const noexcept;

    struct Impl;

private:
    struct Private {};

public:
    FuseMount(Private, std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ghostdrive::vfs
