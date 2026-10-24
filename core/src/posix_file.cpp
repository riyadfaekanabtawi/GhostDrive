#include "posix_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <system_error>

namespace ghostdrive::core::detail {

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.release();
    }
    return *this;
}

UniqueFd::~UniqueFd() {
    if (fd_ >= 0) ::close(fd_);
}

Error errno_error(std::string_view what, const std::filesystem::path& path) {
    const int err = errno;
    Errc code = Errc::io;
    switch (err) {
        case ENOENT:
            code = Errc::not_found;
            break;
        case EEXIST:
            code = Errc::already_exists;
            break;
        case EACCES:
        case EPERM:
            code = Errc::permission;
            break;
        default:
            break;
    }
    return Error{code, std::string(what) + " " + path.string() + ": " +
                           std::generic_category().message(err)};
}

Result<UniqueFd> open_file(const std::filesystem::path& path, int flags, mode_t mode) {
    int fd;
    do {
        fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return errno_error("open", path);
    return UniqueFd(fd);
}

Result<std::uint64_t> file_size(int fd, const std::filesystem::path& path) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) return errno_error("fstat", path);
    return static_cast<std::uint64_t>(st.st_size);
}

Status write_all(int fd, std::span<const std::byte> data, const std::filesystem::path& path) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno_error("write", path);
        }
        data = data.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

Result<std::size_t> pread_full(int fd, std::span<std::byte> out, std::uint64_t offset,
                               const std::filesystem::path& path) {
    std::size_t total = 0;
    while (total < out.size()) {
        const ssize_t n =
            ::pread(fd, out.data() + total, out.size() - total, static_cast<off_t>(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno_error("pread", path);
        }
        if (n == 0) break;
        total += static_cast<std::size_t>(n);
    }
    return total;
}

Status write_new_file(const std::filesystem::path& path, std::span<const std::byte> data,
                      mode_t mode) {
    auto fd = open_file(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (!fd) return fd.error();
    if (auto st = write_all(fd.value().get(), data, path); !st) return st;
    if (::fsync(fd.value().get()) != 0) return errno_error("fsync", path);
    return {};
}

}  // namespace ghostdrive::core::detail
