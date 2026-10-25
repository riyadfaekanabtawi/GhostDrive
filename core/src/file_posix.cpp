#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <system_error>

#include "ghostdrive/core/file.hpp"

namespace ghostdrive::core {

namespace {

// Builds an Error from the current errno, mapping common values to Errc.
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

Result<int> open_fd(const std::filesystem::path& path, int flags, mode_t mode) {
    int fd;
    do {
        fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return errno_error("open", path);
    return fd;
}

}  // namespace

File::File() noexcept : native_(-1) {}

File::File(Native native, std::filesystem::path path) noexcept
    : native_(native), path_(std::move(path)) {}

File::File(File&& other) noexcept : native_(other.native_), path_(std::move(other.path_)) {
    other.native_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (native_ >= 0) ::close(native_);
        native_ = other.native_;
        path_ = std::move(other.path_);
        other.native_ = -1;
    }
    return *this;
}

File::~File() {
    if (native_ >= 0) ::close(native_);
}

bool File::is_open() const noexcept {
    return native_ >= 0;
}

Result<File> File::open_read(const std::filesystem::path& path) {
    auto fd = open_fd(path, O_RDONLY, 0);
    if (!fd) return fd.error();
    return File(fd.value(), path);
}

Result<File> File::create_private(const std::filesystem::path& path) {
    auto fd = open_fd(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (!fd) return fd.error();
    return File(fd.value(), path);
}

Result<File> File::open_scratch(const std::filesystem::path& path) {
    auto fd = open_fd(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (!fd) return fd.error();
    return File(fd.value(), path);
}

Result<File> File::open_private(const std::filesystem::path& path) {
    auto fd = open_fd(path, O_RDONLY | O_NOFOLLOW, 0);
    if (!fd) return fd.error();
    File file(fd.value(), path);
    if (auto st = file.check_private(); !st) return st.error();
    return file;
}

Status File::check_private() const {
    struct stat st{};
    if (::fstat(native_, &st) != 0) return errno_error("fstat", path_);
    if ((st.st_mode & 077) != 0 || st.st_uid != ::geteuid() || !S_ISREG(st.st_mode)) {
        return Error{Errc::permission,
                     path_.string() + " must be a regular file owned by you with mode 0600"};
    }
    return {};
}

Result<std::uint64_t> File::size() const {
    struct stat st{};
    if (::fstat(native_, &st) != 0) return errno_error("fstat", path_);
    return static_cast<std::uint64_t>(st.st_size);
}

Result<std::size_t> File::read_at(std::span<std::byte> out, std::uint64_t offset) const {
    std::size_t total = 0;
    while (total < out.size()) {
        const ssize_t n = ::pread(native_, out.data() + total, out.size() - total,
                                  static_cast<off_t>(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno_error("pread", path_);
        }
        if (n == 0) break;
        total += static_cast<std::size_t>(n);
    }
    return total;
}

Status File::write_at(std::span<const std::byte> data, std::uint64_t offset) {
    while (!data.empty()) {
        const ssize_t n = ::pwrite(native_, data.data(), data.size(), static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno_error("pwrite", path_);
        }
        data = data.subspan(static_cast<std::size_t>(n));
        offset += static_cast<std::uint64_t>(n);
    }
    return {};
}

Status File::truncate(std::uint64_t size) {
    if (::ftruncate(native_, static_cast<off_t>(size)) != 0) return errno_error("ftruncate", path_);
    return {};
}

Status File::sync() {
    if (::fsync(native_) != 0) return errno_error("fsync", path_);
    return {};
}

Status File::close() {
    if (native_ < 0) return {};
    const int fd = native_;
    native_ = -1;
    if (::close(fd) != 0) return errno_error("close", path_);
    return {};
}

Status replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) return errno_error("rename", from);
    return {};
}

Status make_private_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return Error{Errc::io, "create_directories " + dir.string() + ": " + ec.message()};
    if (::chmod(dir.c_str(), 0700) != 0) return errno_error("chmod", dir);
    return {};
}

bool is_private_dir(const std::filesystem::path& dir) {
    struct stat st{};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::geteuid() &&
           (st.st_mode & 077) == 0;
}

}  // namespace ghostdrive::core
