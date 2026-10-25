// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <winioctl.h>
// clang-format on

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include "ghostdrive/core/file.hpp"

namespace ghostdrive::core {

namespace {

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

Error win32_error(std::string_view what, const std::filesystem::path& path,
                  DWORD err = ::GetLastError()) {
    Errc code = Errc::io;
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            code = Errc::not_found;
            break;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            code = Errc::already_exists;
            break;
        case ERROR_ACCESS_DENIED:
            code = Errc::permission;
            break;
        default:
            break;
    }
    return Error{code, std::string(what) + " " + path.string() + ": " +
                           std::system_category().message(static_cast<int>(err))};
}

struct Handle {
    HANDLE h = nullptr;
    ~Handle() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
};

struct LocalMemory {
    PSECURITY_DESCRIPTOR p = nullptr;
    ~LocalMemory() {
        if (p != nullptr) ::LocalFree(p);
    }
};

std::vector<BYTE> token_user_sid(HANDLE token) {
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    if (len == 0) return {};
    std::vector<BYTE> buffer(len);
    if (!::GetTokenInformation(token, TokenUser, buffer.data(), len, &len)) return {};
    PSID sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    std::vector<BYTE> out(::GetLengthSid(sid));
    if (!::CopySid(static_cast<DWORD>(out.size()), out.data(), sid)) return {};
    return out;
}

// The current user's SID, or null if it cannot be read (then nothing counts as private).
PSID current_user() {
    static const std::vector<BYTE> sid = [] {
        Handle token;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token.h)) {
            return std::vector<BYTE>{};
        }
        return token_user_sid(token.h);
    }();
    return sid.empty() ? nullptr : const_cast<BYTE*>(sid.data());
}

// A DACL with one entry: full access for the current user, inherited by children of a directory.
struct PrivateAcl {
    std::vector<BYTE> storage;
    PACL acl() { return reinterpret_cast<PACL>(storage.data()); }
};

bool make_private_acl(PrivateAcl& out, bool inherit) {
    PSID user = current_user();
    if (user == nullptr) return false;
    const DWORD size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
                                          ::GetLengthSid(user));
    out.storage.assign(size, 0);
    const DWORD flags = inherit ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0;
    return ::InitializeAcl(out.acl(), size, ACL_REVISION) &&
           ::AddAccessAllowedAceEx(out.acl(), ACL_REVISION, flags, FILE_ALL_ACCESS, user);
}

// True if `owner` is the current user and `dacl` lets no one else in. Deny entries only take
// access away, and inherit-only entries do not apply to the object itself; any other kind of
// allow entry (object or callback ACEs) is refused rather than interpreted.
bool owner_only(PSID owner, PACL dacl) {
    PSID user = current_user();
    if (user == nullptr || owner == nullptr || !::EqualSid(owner, user)) return false;
    if (dacl == nullptr) return false;  // a null DACL grants everyone full access
    ACL_SIZE_INFORMATION info{};
    if (!::GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) return false;
    for (DWORD i = 0; i < info.AceCount; ++i) {
        void* ace = nullptr;
        if (!::GetAce(dacl, i, &ace)) return false;
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0) continue;
        switch (header->AceType) {
            case ACCESS_DENIED_ACE_TYPE:
                break;
            case ACCESS_ALLOWED_ACE_TYPE: {
                auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
                if (!::EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), user)) return false;
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

Result<File::Native> create(const std::filesystem::path& path, DWORD access, DWORD disposition) {
    PrivateAcl acl;
    SECURITY_DESCRIPTOR sd{};
    if (!make_private_acl(acl, false) ||
        !::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) ||
        !::SetSecurityDescriptorOwner(&sd, current_user(), FALSE) ||
        !::SetSecurityDescriptorDacl(&sd, TRUE, acl.acl(), FALSE) ||
        !::SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        return win32_error("security descriptor for", path);
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    HANDLE h = ::CreateFileW(path.c_str(), access, kShareAll, &sa, disposition,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return win32_error("open", path);
    return h;
}

OVERLAPPED at(std::uint64_t offset) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return ov;
}

constexpr std::size_t kMaxIo = 1u << 30;

}  // namespace

File::File() noexcept : native_(INVALID_HANDLE_VALUE) {}

File::File(Native native, std::filesystem::path path) noexcept
    : native_(native), path_(std::move(path)) {}

File::File(File&& other) noexcept : native_(other.native_), path_(std::move(other.path_)) {
    other.native_ = INVALID_HANDLE_VALUE;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (is_open()) ::CloseHandle(native_);
        native_ = other.native_;
        path_ = std::move(other.path_);
        other.native_ = INVALID_HANDLE_VALUE;
    }
    return *this;
}

File::~File() {
    if (is_open()) ::CloseHandle(native_);
}

bool File::is_open() const noexcept {
    return native_ != INVALID_HANDLE_VALUE;
}

Result<File> File::open_read(const std::filesystem::path& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return win32_error("open", path);
    return File(h, path);
}

Result<File> File::create_private(const std::filesystem::path& path) {
    auto h = create(path, GENERIC_WRITE, CREATE_NEW);
    if (!h) return h.error();
    return File(h.value(), path);
}

Result<File> File::open_scratch(const std::filesystem::path& path) {
    auto h = create(path, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
    if (!h) return h.error();
    File file(h.value(), path);
    // Without this, extending the file allocates every byte up to the new end. Volumes without
    // sparse files (FAT) refuse it; the file then works the same, only less compactly.
    DWORD returned = 0;
    (void)::DeviceIoControl(h.value(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                            nullptr);
    return file;
}

Result<File> File::open_private(const std::filesystem::path& path) {
    // FILE_FLAG_OPEN_REPARSE_POINT opens a symbolic link itself, which check_private refuses,
    // the equivalent of O_NOFOLLOW.
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, kShareAll, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return win32_error("open", path);
    File file(h, path);
    if (auto st = file.check_private(); !st) return st.error();
    return file;
}

Status File::check_private() const {
    const Error refused{Errc::permission, path_.string() +
                                              " must be a regular file owned by you that only "
                                              "you can access"};
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileType(native_) != FILE_TYPE_DISK || !::GetFileInformationByHandle(native_, &info)) {
        return refused;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return refused;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    LocalMemory sd;
    const DWORD err = ::GetSecurityInfo(native_, SE_FILE_OBJECT,
                                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        &owner, nullptr, &dacl, nullptr, &sd.p);
    if (err != ERROR_SUCCESS) return win32_error("GetSecurityInfo", path_, err);
    if (!owner_only(owner, dacl)) return refused;
    return {};
}

Result<std::uint64_t> File::size() const {
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(native_, &size)) return win32_error("GetFileSizeEx", path_);
    return static_cast<std::uint64_t>(size.QuadPart);
}

Result<std::size_t> File::read_at(std::span<std::byte> out, std::uint64_t offset) const {
    std::size_t total = 0;
    while (total < out.size()) {
        OVERLAPPED ov = at(offset + total);
        const auto want = static_cast<DWORD>(std::min(out.size() - total, kMaxIo));
        DWORD n = 0;
        if (!::ReadFile(native_, out.data() + total, want, &n, &ov)) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            return win32_error("ReadFile", path_, err);
        }
        if (n == 0) break;
        total += n;
    }
    return total;
}

Status File::write_at(std::span<const std::byte> data, std::uint64_t offset) {
    while (!data.empty()) {
        OVERLAPPED ov = at(offset);
        const auto want = static_cast<DWORD>(std::min(data.size(), kMaxIo));
        DWORD n = 0;
        if (!::WriteFile(native_, data.data(), want, &n, &ov)) {
            return win32_error("WriteFile", path_);
        }
        data = data.subspan(n);
        offset += n;
    }
    return {};
}

Status File::truncate(std::uint64_t size) {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!::SetFileInformationByHandle(native_, FileEndOfFileInfo, &info, sizeof(info))) {
        return win32_error("SetEndOfFile", path_);
    }
    return {};
}

Status File::sync() {
    if (!::FlushFileBuffers(native_)) return win32_error("FlushFileBuffers", path_);
    return {};
}

Status File::close() {
    if (!is_open()) return {};
    HANDLE h = native_;
    native_ = INVALID_HANDLE_VALUE;
    if (!::CloseHandle(h)) return win32_error("CloseHandle", path_);
    return {};
}

Status replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!::MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return win32_error("rename", from);
    }
    return {};
}

Status make_private_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return Error{Errc::io, "create_directories " + dir.string() + ": " + ec.message()};
    PrivateAcl acl;
    if (!make_private_acl(acl, true)) return win32_error("access list for", dir);
    std::wstring name = dir.wstring();
    // Protected: the parent's inheritable entries no longer apply. Existing children that
    // inherit pick up the new entry; files created with their own protected DACL keep it.
    const DWORD err =
        ::SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                    PROTECTED_DACL_SECURITY_INFORMATION,
                                current_user(), nullptr, acl.acl(), nullptr);
    if (err != ERROR_SUCCESS) return win32_error("SetNamedSecurityInfo", dir, err);
    return {};
}

bool is_private_dir(const std::filesystem::path& dir) {
    const DWORD attributes = ::GetFileAttributesW(dir.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    LocalMemory sd;
    if (::GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                nullptr, &dacl, nullptr, &sd.p) != ERROR_SUCCESS) {
        return false;
    }
    return owner_only(owner, dacl);
}

bool process_is_current_user(unsigned long pid) {
    PSID user = current_user();
    if (user == nullptr) return false;
    Handle process{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (process.h == nullptr) return false;
    Handle token;
    if (!::OpenProcessToken(process.h, TOKEN_QUERY, &token.h)) return false;
    auto sid = token_user_sid(token.h);
    return !sid.empty() && ::EqualSid(sid.data(), user);
}

}  // namespace ghostdrive::core
