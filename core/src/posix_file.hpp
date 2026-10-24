#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "ghostdrive/core/result.hpp"

namespace ghostdrive::core::detail {

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd();

    int get() const noexcept { return fd_; }
    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_ = -1;
};

// Builds an Error from the current errno, mapping common values to Errc.
Error errno_error(std::string_view what, const std::filesystem::path& path);

Result<UniqueFd> open_file(const std::filesystem::path& path, int flags, mode_t mode = 0);
Result<std::uint64_t> file_size(int fd, const std::filesystem::path& path);
Status write_all(int fd, std::span<const std::byte> data, const std::filesystem::path& path);
// Reads until out is full or end of file; returns the number of bytes read.
Result<std::size_t> pread_full(int fd, std::span<std::byte> out, std::uint64_t offset,
                               const std::filesystem::path& path);
// Creates a new file (O_EXCL) with the given mode, writes data, and fsyncs it.
Status write_new_file(const std::filesystem::path& path, std::span<const std::byte> data,
                      mode_t mode);

}  // namespace ghostdrive::core::detail
