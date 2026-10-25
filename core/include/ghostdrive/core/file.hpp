#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/result.hpp"

// Files on the local disk, and files and directories private to the current user. Private
// means mode 0600 (files) or 0700 (directories) and owned by the effective user on POSIX. On
// Windows it means owned by the current user's SID with a protected DACL whose only entry grants
// that SID access; a private directory's entry is inherited, so files created in it by any code
// (SQLite, std::ofstream) are private too.
namespace ghostdrive::core {

// An open file with positional reads and writes, closed on destruction.
class File {
public:
#ifdef _WIN32
    using Native = void*;  // HANDLE
#else
    using Native = int;
#endif

    File() noexcept;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File();

    // An existing file, read-only.
    static Result<File> open_read(const std::filesystem::path& path);
    // A new private file, write-only; Errc::already_exists if the path exists.
    static Result<File> create_private(const std::filesystem::path& path);
    // A private read-write file, created or emptied. Sparse where the platform needs asking.
    static Result<File> open_scratch(const std::filesystem::path& path);
    // An existing private file, read-only. A symbolic link is not followed, and anything but a
    // regular file that is private to the current user is refused with Errc::permission.
    static Result<File> open_private(const std::filesystem::path& path);

    bool is_open() const noexcept;
    const std::filesystem::path& path() const noexcept { return path_; }

    Result<std::uint64_t> size() const;
    // Reads until out is full or end of file; returns the number of bytes read.
    Result<std::size_t> read_at(std::span<std::byte> out, std::uint64_t offset) const;
    Status write_at(std::span<const std::byte> data, std::uint64_t offset);
    Status truncate(std::uint64_t size);
    // Flushes written data to the device (fsync, FlushFileBuffers).
    Status sync();
    // Closes now, reporting an error the destructor would have to ignore.
    Status close();

private:
    File(Native native, std::filesystem::path path) noexcept;
    Status check_private() const;

    Native native_;
    std::filesystem::path path_;
};

// Replaces `path` with `data` through a private temporary file, flushed and renamed into place,
// so a crash never leaves a partial file and the data is never readable by others, even briefly.
Status write_private_file(const std::filesystem::path& path, std::span<const std::byte> data);
// The whole content of a file opened with File::open_private.
Result<Bytes> read_private_file(const std::filesystem::path& path);
// Renames `from` to `to`, replacing `to` if it exists.
Status replace_file(const std::filesystem::path& from, const std::filesystem::path& to);

// `path` in UTF-8, for messages and for interfaces that take UTF-8 names (SQLite, AF_UNIX on
// Windows). Unlike path::string(), which on Windows converts to the ANSI code page and throws
// for a character outside it, this does not throw. POSIX paths are returned as they are.
// path_utf8 replaces what UTF-8 cannot hold (an unpaired UTF-16 surrogate, Windows only) with
// U+FFFD; exact_utf8 refuses such a path with Errc::invalid_argument.
std::string path_utf8(const std::filesystem::path& path);
Result<std::string> exact_utf8(const std::filesystem::path& path);

// Creates `dir` (and its parents) if needed and makes it private.
Status make_private_dir(const std::filesystem::path& dir);
// True if `dir` is a directory private to the current user.
bool is_private_dir(const std::filesystem::path& dir);

#ifdef _WIN32
// True if the process `pid` runs as the current user. Windows AF_UNIX sockets have no peer
// credentials, only the peer's process ID (SIO_AF_UNIX_GETPEERPID).
bool process_is_current_user(unsigned long pid);
#endif

}  // namespace ghostdrive::core
