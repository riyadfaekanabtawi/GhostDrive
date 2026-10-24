#pragma once

#include <asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ghostdrive/core/result.hpp"
#include "ghostdrive/vfs/fetcher.hpp"
#include "ghostdrive/vfs/file_view.hpp"
#include "ghostdrive/vfs/tree.hpp"

namespace ghostdrive::vfs {

struct FilesystemOptions {
    // A read that has not completed by then fails with Errc::timeout; the chunk fetch keeps
    // going so the cache still benefits.
    std::chrono::milliseconds request_timeout{30'000};
    std::size_t max_readahead_chunks = 8;
};

// Backend-independent filesystem operations for the FUSE adapters. Metadata calls are
// synchronous and thread-safe. Reads run on the fetcher's executor and complete through a
// callback there, so a FUSE thread never blocks on the network.
class Filesystem {
public:
    using ReadCallback = std::function<void(core::Result<ReadReply>)>;

    Filesystem(asio::any_io_executor io, Tree& tree, Fetcher& fetcher,
               FilesystemOptions options = {});

    std::optional<Ino> lookup(Ino parent, std::string_view name) const {
        return tree_.lookup(parent, name);
    }
    std::optional<Attr> getattr(Ino ino) const { return tree_.getattr(ino); }
    std::optional<std::vector<DirEntry>> readdir(Ino ino) const { return tree_.readdir(ino); }

    // Pins the file's current version and returns a handle. Fails with not_found for missing or
    // non-file inodes.
    core::Result<std::uint64_t> open(Ino ino);
    void release(std::uint64_t handle);
    // The callback runs exactly once, on the io executor.
    void read(std::uint64_t handle, std::uint64_t offset, std::size_t size, ReadCallback done);

    std::size_t open_handles() const;

private:
    asio::any_io_executor io_;
    Tree& tree_;
    Fetcher& fetcher_;
    FilesystemOptions options_;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<FileView>> handles_;
    std::uint64_t next_handle_ = 1;
};

// errno for an error reported to the kernel.
int to_errno(const core::Error& error) noexcept;

}  // namespace ghostdrive::vfs
