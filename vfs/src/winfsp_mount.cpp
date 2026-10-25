// WinFsp adapter over the native API (FSP_FILE_SYSTEM_INTERFACE), checked against the WinFsp
// 2.1 headers (inc/winfsp/winfsp.h, inc/winfsp/fsctl.h). Reads and writes return STATUS_PENDING
// and are answered with FspFileSystemSendResponse when the Filesystem completes them on its
// executor; WinFsp allows that only for Read, Write and ReadDirectory, so the other operations
// wait for the Filesystem on the dispatcher thread.

// winfsp.h includes windows.h with WIN32_NO_STATUS and then ntstatus.h; it must come first.
// It also uses PNTSTATUS, which windows.h does not declare under WIN32_LEAN_AND_MEAN (set for
// the whole project), so those headers are included here first, the same way, and the type
// declared after them.
// clang-format off
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
typedef NTSTATUS* PNTSTATUS;
#include <winfsp/winfsp.h>
#include <sddl.h>
// clang-format on

#include "ghostdrive/vfs/winfsp_mount.hpp"

#include "ghostdrive/core/file.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <future>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;

namespace {

constexpr UINT64 kUnixEpochAsFileTime = 116444736000000000ULL;  // 1970-01-01 in 100 ns units
constexpr ULONG kMaxNameChars = 255;

UINT64 now_filetime() {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    return (static_cast<UINT64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Strict conversions: a name that is not valid UTF-16 (or UTF-8) has no counterpart.
std::optional<std::string> to_utf8(std::wstring_view wide) {
    if (wide.empty()) return std::string();
    const int in = static_cast<int>(wide.size());
    const int n = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), in, nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return std::nullopt;
    std::string out(static_cast<std::size_t>(n), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), in, out.data(), n,
                              nullptr, nullptr) != n) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::wstring> to_wide(std::string_view utf8) {
    if (utf8.empty()) return std::wstring();
    const int in = static_cast<int>(utf8.size());
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), in, nullptr, 0);
    if (n <= 0) return std::nullopt;
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), in, out.data(), n) != n) {
        return std::nullopt;
    }
    return out;
}

bool equal_ignoring_case(std::wstring_view a, std::wstring_view b) {
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                  static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// The key two names share when Windows treats them as the same name: uppercase by file system
// rules (LCMapStringEx without LCMAP_LINGUISTIC_CASING, documented as the file system casing).
std::wstring fold(std::wstring_view name) {
    if (name.empty()) return {};
    const int in = static_cast<int>(name.size());
    const int n = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, name.data(), in, nullptr,
                                  0, nullptr, nullptr, 0);
    if (n <= 0) return std::wstring(name);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    if (::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, name.data(), in, out.data(), n,
                        nullptr, nullptr, 0) != n) {
        return std::wstring(name);
    }
    return out;
}

// A name Windows can create and open: no control or reserved characters, no trailing dot or
// space, and not a device name (CON, NUL, COM1, ...), with or without an extension. Names from
// other platforms that break these rules are left out of listings.
bool windows_name_ok(std::wstring_view name) {
    if (name.empty() || name.size() > kMaxNameChars || name == L"." || name == L"..") return false;
    for (wchar_t c : name) {
        if (c < 0x20 || std::wcschr(L"<>:\"/\\|?*", c) != nullptr) return false;
    }
    if (name.back() == L'.' || name.back() == L' ') return false;
    std::wstring_view base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.remove_suffix(1);
    for (const wchar_t* device : {L"CON", L"PRN", L"AUX", L"NUL"}) {
        if (equal_ignoring_case(base, device)) return false;
    }
    if (base.size() == 4 && base[3] >= L'1' && base[3] <= L'9' &&
        (equal_ignoring_case(base.substr(0, 3), L"COM") ||
         equal_ignoring_case(base.substr(0, 3), L"LPT"))) {
        return false;
    }
    return true;
}

NTSTATUS to_ntstatus(const Error& error) {
    switch (error.code) {
        case Errc::not_found:
            return STATUS_OBJECT_NAME_NOT_FOUND;
        case Errc::already_exists:
            return STATUS_OBJECT_NAME_COLLISION;
        case Errc::invalid_argument:
            return STATUS_INVALID_PARAMETER;
        case Errc::permission:
            return STATUS_ACCESS_DENIED;
        case Errc::timeout:
            return STATUS_IO_TIMEOUT;
        case Errc::closed:
            return STATUS_CONNECTION_DISCONNECTED;
        case Errc::not_empty:
            return STATUS_DIRECTORY_NOT_EMPTY;
        case Errc::is_directory:
            return STATUS_FILE_IS_A_DIRECTORY;
        case Errc::not_directory:
            return STATUS_NOT_A_DIRECTORY;
        case Errc::read_only:
            return STATUS_MEDIA_WRITE_PROTECTED;
        case Errc::io:
        case Errc::corrupt:
        case Errc::crypto:
        case Errc::database:
        case Errc::protocol:
            break;
    }
    return STATUS_UNEXPECTED_IO_ERROR;
}

NTSTATUS to_ntstatus(const core::Status& st) {
    return st ? STATUS_SUCCESS : to_ntstatus(st.error());
}

// Runs `start`, which hands its callback to the Filesystem, and waits for the result. Only for
// operations WinFsp cannot complete asynchronously; the dispatcher has several threads.
template <typename T, typename Start>
T wait_for(Start start) {
    std::promise<T> promise;
    auto future = promise.get_future();
    start([&promise](T value) { promise.set_value(std::move(value)); });
    return future.get();
}

// No exception may unwind into WinFsp's dispatcher.
template <typename F>
NTSTATUS guarded(F&& body) noexcept {
    try {
        return body();
    } catch (...) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

}  // namespace

struct WinFspMount::Impl {
    Impl(Filesystem& fs, std::filesystem::path mountpoint, WinFspMountOptions options)
        : fs(fs), mountpoint(std::move(mountpoint)), options(std::move(options)) {}
    ~Impl() {
        if (security != nullptr) ::LocalFree(security);
    }

    Filesystem& fs;
    std::filesystem::path mountpoint;
    WinFspMountOptions options;
    FSP_FILE_SYSTEM* fsp = nullptr;
    bool mounted = false;
    UINT64 mounted_at = now_filetime();
    std::wstring label;
    // Owner-only, self-relative: returned for every file and directory, and used for the
    // mountpoint itself.
    PSECURITY_DESCRIPTOR security = nullptr;
    ULONG security_size = 0;

    // Reads and writes answered later; unmount waits for them before deleting the volume.
    std::mutex pending_mutex;
    std::condition_variable pending_done;
    int pending = 0;

    void begin_pending() {
        std::lock_guard lock(pending_mutex);
        ++pending;
    }
    void end_pending() {
        std::lock_guard lock(pending_mutex);
        if (--pending == 0) pending_done.notify_all();
    }

    // A pending request's buffer is mapped into this process until the request is answered or
    // the volume's I/O queue is stopped with its requests cancelled (FSP_FSCTL_STOP, sent by
    // FspFileSystemStopDispatcher), whichever comes first. A reader's cancel or exit and the IRP
    // timeout do not end it: the driver is built with FSP_IOQ_PROCESS_NO_CANCEL, so a request
    // the file system has received has no cancel routine (WinFsp 2.1 src/sys/ioq.c,
    // FspIoqStartProcessingIrp; the unmapping is FspFsvolReadNonCachedRequestFini in read.c).
    // A late reply touches the buffer only while holding `replies` shared and seeing
    // `replies_open`; unmount clears it under `replies` exclusively before stopping the
    // dispatcher.
    std::shared_mutex replies;
    bool replies_open = true;

    // Opens WinFsp has not closed yet; unmount commits and releases what remains.
    std::mutex nodes_mutex;
    std::unordered_set<void*> nodes;
};

namespace {

using Impl = WinFspMount::Impl;

Impl& impl_of(FSP_FILE_SYSTEM* fsp) {
    return *static_cast<Impl*>(fsp->UserContext);
}

// One per Create or Open (the volume sets UmFileContextIsUserContext2).
struct Node {
    Ino ino = 0;
    Ino parent = 0;
    bool directory = false;
    bool write = false;
    std::optional<std::uint64_t> handle;  // files only
    // Answers GetFileInfo once the entry is gone (deleted while open).
    std::mutex last_mutex;
    Attr last;

    // Directory listing, taken when an enumeration starts so markers stay meaningful.
    std::mutex listing_mutex;
    std::vector<std::pair<std::wstring, Ino>> listing;
};

Node& node_of(PVOID context) {
    return *static_cast<Node*>(context);
}

void fill(const Impl& self, const Attr& attr, FSP_FSCTL_FILE_INFO* info) {
    std::memset(info, 0, sizeof(*info));
    if (attr.type == NodeType::directory) {
        info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        info->FileAttributes =
            (attr.perm & 0222) == 0 ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_ARCHIVE;
        info->FileSize = attr.size;
        info->AllocationSize = (attr.size + 4095) / 4096 * 4096;
    }
    const UINT64 time = attr.mtime_ns > 0
                            ? kUnixEpochAsFileTime + static_cast<UINT64>(attr.mtime_ns) / 100
                            : self.mounted_at;
    info->CreationTime = time;
    info->LastAccessTime = time;
    info->LastWriteTime = time;
    info->ChangeTime = time;
    info->IndexNumber = attr.ino;
}

struct Child {
    Ino ino;
    std::string name;  // as stored, which may differ in case from the name asked for
};

// Windows looks names up without regard to case, and WinFsp keeps one kernel file node per name
// compared that way. So of the entries in a directory whose names differ only in case (possible
// from POSIX devices), exactly one is visible: the first in the Tree's order, which sorts names
// by their UTF-8 bytes ("Report.txt" before "report.txt"). Every spelling resolves to it, even
// the exact name of a hidden one, and listings show only it.
std::optional<Child> find_child(const Filesystem& fs, Ino parent, std::wstring_view name) {
    auto entries = fs.readdir(parent);
    if (!entries) return std::nullopt;
    const std::wstring key = fold(name);
    for (auto& e : *entries) {
        auto wide = to_wide(e.name);
        if (wide && fold(*wide) == key) return Child{e.ino, std::move(e.name)};
    }
    return std::nullopt;
}

struct Found {
    Ino ino = kRootIno;
    Ino parent = kRootIno;
    std::string name;  // stored name of the last component; empty for the root
    Attr attr;
};

// Resolves a WinFsp file name ("\", "\dir\file"). A missing last component is
// STATUS_OBJECT_NAME_NOT_FOUND, a missing or non-directory one before it
// STATUS_OBJECT_PATH_NOT_FOUND.
NTSTATUS resolve(const Filesystem& fs, std::wstring_view path, Found& out) {
    out = Found{};
    auto root = fs.getattr(kRootIno);
    if (!root) return STATUS_OBJECT_PATH_NOT_FOUND;
    out.attr = *root;
    std::size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == L'\\') ++pos;
        if (pos == path.size()) break;
        std::size_t end = path.find(L'\\', pos);
        if (end == std::wstring_view::npos) end = path.size();
        const bool last = path.find_first_not_of(L'\\', end) == std::wstring_view::npos;
        if (out.attr.type != NodeType::directory) return STATUS_OBJECT_PATH_NOT_FOUND;
        auto child = find_child(fs, out.ino, path.substr(pos, end - pos));
        auto attr = child ? fs.getattr(child->ino) : std::nullopt;
        if (!attr) return last ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
        out.parent = out.ino;
        out.ino = child->ino;
        out.name = std::move(child->name);
        out.attr = *attr;
        pos = end;
    }
    return STATUS_SUCCESS;
}

// "\dir\file" -> ("\dir", "file").
std::pair<std::wstring_view, std::wstring_view> split(std::wstring_view path) {
    while (path.size() > 1 && path.back() == L'\\') path.remove_suffix(1);
    const std::size_t slash = path.rfind(L'\\');
    if (slash == std::wstring_view::npos) return {L"\\", path};
    return {slash == 0 ? std::wstring_view(L"\\") : path.substr(0, slash), path.substr(slash + 1)};
}

NTSTATUS copy_security(const Impl& self, PSECURITY_DESCRIPTOR out, SIZE_T* size) {
    if (size == nullptr) return STATUS_SUCCESS;
    if (*size < self.security_size) {
        *size = self.security_size;
        return STATUS_BUFFER_OVERFLOW;
    }
    *size = self.security_size;
    if (out != nullptr) std::memcpy(out, self.security, self.security_size);
    return STATUS_SUCCESS;
}

NTSTATUS op_get_volume_info(FSP_FILE_SYSTEM* fsp, FSP_FSCTL_VOLUME_INFO* info) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        std::memset(info, 0, sizeof(*info));
        ULARGE_INTEGER free_to_caller{};
        ULARGE_INTEGER total{};
        if (!self.options.data_dir.empty() &&
            ::GetDiskFreeSpaceExW(self.options.data_dir.c_str(), &free_to_caller, &total,
                                  nullptr)) {
            info->TotalSize = total.QuadPart;
            info->FreeSize = free_to_caller.QuadPart;
        }
        const std::size_t chars =
            std::min(self.label.size(), sizeof(info->VolumeLabel) / sizeof(WCHAR));
        std::memcpy(info->VolumeLabel, self.label.data(), chars * sizeof(WCHAR));
        info->VolumeLabelLength = static_cast<UINT16>(chars * sizeof(WCHAR));
        return STATUS_SUCCESS;
    });
}

NTSTATUS op_get_security_by_name(FSP_FILE_SYSTEM* fsp, PWSTR name, PUINT32 attributes,
                                 PSECURITY_DESCRIPTOR security, SIZE_T* security_size) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        Found found;
        if (NTSTATUS st = resolve(self.fs, name, found); !NT_SUCCESS(st)) return st;
        if (attributes != nullptr) {
            FSP_FSCTL_FILE_INFO info;
            fill(self, found.attr, &info);
            *attributes = info.FileAttributes;
        }
        return copy_security(self, security, security_size);
    });
}

// Hands a new open to WinFsp with the system cache off for it. The FSD enables the cache only
// when FileInfoTimeout is INFINITE and Rsp.Create.Opened.DisableCache is clear (fsctl.h,
// FSP_FSCTL_TRANSACT_RSP; src/sys/create.c, FspFsvolCreateComplete); the volume's timeout is
// finite and this sets the flag as well, so every read and write reaches the Filesystem and
// nothing is written back after Cleanup.
NTSTATUS opened(Impl& self, std::unique_ptr<Node> node, PVOID* context, FSP_FSCTL_FILE_INFO* info) {
    fill(self, node->last, info);
    FspFileSystemGetOperationContext()->Response->Rsp.Create.Opened.DisableCache = 1;
    {
        std::lock_guard lock(self.nodes_mutex);
        self.nodes.insert(node.get());
    }
    *context = node.release();
    return STATUS_SUCCESS;
}

NTSTATUS op_create(FSP_FILE_SYSTEM* fsp, PWSTR name, UINT32 create_options, UINT32, UINT32,
                   PSECURITY_DESCRIPTOR, UINT64, PVOID* context, FSP_FSCTL_FILE_INFO* info) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        auto [parent_path, leaf] = split(name);
        Found parent;
        NTSTATUS st = resolve(self.fs, parent_path, parent);
        if (!NT_SUCCESS(st) || parent.attr.type != NodeType::directory) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        auto utf8 = to_utf8(leaf);
        if (!windows_name_ok(leaf) || !utf8) return STATUS_OBJECT_NAME_INVALID;
        if (find_child(self.fs, parent.ino, leaf)) return STATUS_OBJECT_NAME_COLLISION;

        auto node = std::make_unique<Node>();
        node->parent = parent.ino;
        if ((create_options & FILE_DIRECTORY_FILE) != 0) {
            auto attr = self.fs.mkdir(parent.ino, *utf8, 0755);
            if (!attr) return to_ntstatus(attr.error());
            node->ino = attr.value().ino;
            node->directory = true;
            node->last = attr.value();
        } else {
            auto created = self.fs.create(parent.ino, *utf8, 0644);
            if (!created) return to_ntstatus(created.error());
            node->ino = created.value().first.ino;
            node->write = true;
            node->handle = created.value().second;
            node->last = created.value().first;
        }
        return opened(self, std::move(node), context, info);
    });
}

NTSTATUS op_open(FSP_FILE_SYSTEM* fsp, PWSTR name, UINT32, UINT32 granted, PVOID* context,
                 FSP_FSCTL_FILE_INFO* info) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        Found found;
        if (NTSTATUS st = resolve(self.fs, name, found); !NT_SUCCESS(st)) return st;
        auto node = std::make_unique<Node>();
        node->ino = found.ino;
        node->parent = found.parent;
        node->last = found.attr;
        if (found.attr.type == NodeType::directory) {
            node->directory = true;
        } else {
            // As O_WRONLY or O_RDWR would: a handle that may write gets a staging file. WinFsp
            // grants write access on a read-only volume too, so it is refused here.
            node->write = (granted & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
            if (node->write && !self.fs.writable()) return STATUS_MEDIA_WRITE_PROTECTED;
            auto handle = self.fs.open(found.ino, node->write, false);
            if (!handle) return to_ntstatus(handle.error());
            node->handle = handle.value();
        }
        return opened(self, std::move(node), context, info);
    });
}

// The current attributes of an open node, or the last known ones if it was deleted.
Attr current(Impl& self, Node& node) {
    auto attr = self.fs.getattr(node.ino);
    std::lock_guard lock(node.last_mutex);
    if (attr) node.last = *attr;
    return node.last;
}

NTSTATUS set_attr(Impl& self, Node& node, SetAttr changes, FSP_FSCTL_FILE_INFO* info) {
    if (changes.size || changes.perm || changes.mtime_ns) {
        auto result = wait_for<core::Result<Attr>>(
            [&](auto done) { self.fs.setattr(node.ino, changes, node.handle, std::move(done)); });
        if (!result) return to_ntstatus(result.error());
    }
    fill(self, current(self, node), info);
    return STATUS_SUCCESS;
}

NTSTATUS op_overwrite(FSP_FILE_SYSTEM* fsp, PVOID context, UINT32, BOOLEAN, UINT64,
                      FSP_FSCTL_FILE_INFO* info) {
    return guarded([&] {
        SetAttr changes;
        changes.size = 0;
        return set_attr(impl_of(fsp), node_of(context), changes, info);
    });
}

// Cleanup is the last CloseHandle on a file object: the close(2) of POSIX. Pending writes are
// committed here, and a delete (DeleteFile, delete-on-close) takes effect. Windows has no way
// to report a failure from it.
void op_cleanup(FSP_FILE_SYSTEM* fsp, PVOID context, PWSTR name, ULONG flags) {
    (void)guarded([&] {
        auto& self = impl_of(fsp);
        auto& node = node_of(context);
        if ((flags & FspCleanupDelete) != 0 && name != nullptr) {
            auto [parent_path, leaf] = split(name);
            Found parent;
            if (!NT_SUCCESS(resolve(self.fs, parent_path, parent))) return STATUS_SUCCESS;
            auto child = find_child(self.fs, parent.ino, leaf);
            if (!child || child->ino != node.ino) return STATUS_SUCCESS;
            // A deleted file's open writers are orphaned, so nothing commits it back.
            (void)(node.directory ? self.fs.rmdir(parent.ino, child->name)
                                  : self.fs.unlink(parent.ino, child->name));
            return STATUS_SUCCESS;
        }
        if (node.write && node.handle) {
            (void)wait_for<core::Status>(
                [&](auto done) { self.fs.flush(*node.handle, std::move(done)); });
        }
        return STATUS_SUCCESS;
    });
}

// Close follows the last Cleanup once the system holds no more references (the section cache
// included): the handle is released, committing anything still pending in the background.
void op_close(FSP_FILE_SYSTEM* fsp, PVOID context) {
    std::unique_ptr<Node> node(static_cast<Node*>(context));
    (void)guarded([&] {
        auto& self = impl_of(fsp);
        {
            std::lock_guard lock(self.nodes_mutex);
            self.nodes.erase(context);
        }
        if (node->handle) self.fs.release(*node->handle);
        return STATUS_SUCCESS;
    });
}

// Sends a late reply. `touch_buffer` runs first, both only while the request's buffer is still
// mapped (see Impl::replies); after unmount has begun, neither runs.
template <typename F>
void reply_later(Impl& self, FSP_FSCTL_TRANSACT_RSP& response, F&& touch_buffer) {
    std::shared_lock gate(self.replies);
    if (!self.replies_open) return;
    touch_buffer();
    FspFileSystemSendResponse(self.fsp, &response);
}

// Starts an operation that answers later. It counts as pending from before it is handed to the
// Filesystem, whose callback ends it, and stops counting if handing it over throws.
template <typename Start>
NTSTATUS start_pending(Impl& self, Start start) {
    self.begin_pending();
    try {
        start();
    } catch (...) {
        self.end_pending();
        throw;
    }
    return STATUS_PENDING;
}

NTSTATUS op_read(FSP_FILE_SYSTEM* fsp, PVOID context, PVOID buffer, UINT64 offset, ULONG length,
                 PULONG transferred) {
    return guarded([&]() -> NTSTATUS {
        auto& self = impl_of(fsp);
        auto& node = node_of(context);
        if (!node.handle) return STATUS_INVALID_DEVICE_REQUEST;
        if (length == 0) {
            *transferred = 0;
            return STATUS_SUCCESS;
        }
        const UINT64 hint = FspFileSystemGetOperationContext()->Request->Hint;
        // Returns without answering; the callback answers from the Filesystem's executor, so
        // this dispatcher thread is free while the chunk is fetched and verified.
        return start_pending(self, [&] {
            self.fs.read(
                *node.handle, offset, length,
                [&self, buffer, length, hint](core::Result<ReadReply> result) {
                    FSP_FSCTL_TRANSACT_RSP response{};
                    response.Size = sizeof(response);
                    response.Kind = FspFsctlTransactReadKind;
                    response.Hint = hint;
                    reply_later(self, response, [&] {
                        if (!result) {
                            response.IoStatus.Status =
                                static_cast<UINT32>(result.error().code == Errc::invalid_argument
                                                        ? STATUS_INVALID_HANDLE
                                                        : STATUS_UNEXPECTED_IO_ERROR);
                            return;
                        }
                        auto* out = static_cast<std::byte*>(buffer);
                        std::size_t used = 0;
                        for (auto slice : result.value().slices) {
                            const std::size_t n =
                                std::min<std::size_t>(slice.size(), length - used);
                            // COPY: into the request's read buffer, which WinFsp maps from the
                            // reader (C8).
                            std::memcpy(out + used, slice.data(), n);
                            used += n;
                        }
                        response.IoStatus.Status =
                            static_cast<UINT32>(used == 0 ? STATUS_END_OF_FILE : STATUS_SUCCESS);
                        response.IoStatus.Information = static_cast<UINT32>(used);
                    });
                    self.end_pending();
                });
        });
    });
}

// Copies at most this much of a write request at a time, so a huge non-cached write does not
// need a buffer of its size; the pieces go to the Filesystem in order.
constexpr ULONG kMaxWritePiece = 4u << 20;

struct PendingWrite {
    std::uint64_t handle = 0;
    Ino ino = 0;
    Attr before;
    const std::byte* source = nullptr;  // the request's buffer, mapped until it is answered
    UINT64 offset = 0;
    ULONG length = 0;
    ULONG done = 0;
    UINT64 hint = 0;
};

void write_next(Impl& self, std::shared_ptr<PendingWrite> job);

void finish_write(Impl& self, const PendingWrite& job, const core::Status& st) {
    FSP_FSCTL_TRANSACT_RSP response{};
    response.Size = sizeof(response);
    response.Kind = FspFsctlTransactWriteKind;
    response.Hint = job.hint;
    response.IoStatus.Status = static_cast<UINT32>(to_ntstatus(st));
    if (st) {
        response.IoStatus.Information = job.length;
        auto attr = self.fs.getattr(job.ino);
        if (!attr) {
            attr = job.before;
            attr->size = std::max<std::uint64_t>(attr->size, job.offset + job.length);
        }
        fill(self, *attr, &response.Rsp.Write.FileInfo);
    }
    reply_later(self, response, [] {});
    self.end_pending();
}

void write_next(Impl& self, std::shared_ptr<PendingWrite> job) {
    core::Bytes data;
    {
        std::shared_lock gate(self.replies);
        if (!self.replies_open) {  // unmounting: the buffer may be gone, and no reply is wanted
            self.end_pending();
            return;
        }
        const ULONG n = std::min(job->length - job->done, kMaxWritePiece);
        // COPY: the request buffer is only valid until the response, and the Filesystem takes
        // the bytes by value to write them later on its executor.
        data.assign(job->source + job->done, job->source + job->done + n);
    }
    const auto n = static_cast<ULONG>(data.size());
    const UINT64 at = job->offset + job->done;
    self.fs.write(job->handle, at, std::move(data), [&self, job, n](core::Status st) {
        if (st) job->done += n;
        if (!st || job->done == job->length) {
            finish_write(self, *job, st);
            return;
        }
        try {
            write_next(self, job);
        } catch (...) {
            finish_write(self, *job, Error{Errc::io, "out of memory"});
        }
    });
}

NTSTATUS op_write(FSP_FILE_SYSTEM* fsp, PVOID context, PVOID buffer, UINT64 offset, ULONG length,
                  BOOLEAN to_end, BOOLEAN constrained, PULONG transferred,
                  FSP_FSCTL_FILE_INFO* info) {
    return guarded([&]() -> NTSTATUS {
        auto& self = impl_of(fsp);
        auto& node = node_of(context);
        if (!node.handle || !node.write) return STATUS_INVALID_DEVICE_REQUEST;
        const Attr before = current(self, node);
        const std::uint64_t size = before.size;
        if (constrained) {
            // Paging I/O must not extend the file.
            if (offset >= size) {
                *transferred = 0;
                fill(self, before, info);
                return STATUS_SUCCESS;
            }
            length = static_cast<ULONG>(std::min<UINT64>(length, size - offset));
        } else if (to_end) {
            offset = size;
        }
        if (length == 0) {
            *transferred = 0;
            fill(self, before, info);
            return STATUS_SUCCESS;
        }
        // Append-to-end takes the size seen here, so two handles appending at once are not
        // atomic with respect to each other.
        auto job = std::make_shared<PendingWrite>();
        job->handle = *node.handle;
        job->ino = node.ino;
        job->before = before;
        job->source = static_cast<const std::byte*>(buffer);
        job->offset = offset;
        job->length = length;
        job->hint = FspFileSystemGetOperationContext()->Request->Hint;
        return start_pending(self, [&] { write_next(self, std::move(job)); });
    });
}

NTSTATUS op_flush(FSP_FILE_SYSTEM* fsp, PVOID context, FSP_FSCTL_FILE_INFO* info) {
    return guarded([&] {
        if (context == nullptr) return STATUS_SUCCESS;  // the whole volume: nothing is buffered
        auto& self = impl_of(fsp);
        auto& node = node_of(context);
        if (node.write && node.handle) {
            auto st = wait_for<core::Status>(
                [&](auto done) { self.fs.flush(*node.handle, std::move(done)); });
            if (!st) return to_ntstatus(st);
        }
        fill(self, current(self, node), info);
        return STATUS_SUCCESS;
    });
}

NTSTATUS op_get_file_info(FSP_FILE_SYSTEM* fsp, PVOID context, FSP_FSCTL_FILE_INFO* info) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        fill(self, current(self, node_of(context)), info);
        return STATUS_SUCCESS;
    });
}

NTSTATUS op_set_basic_info(FSP_FILE_SYSTEM* fsp, PVOID context, UINT32, UINT64, UINT64,
                           UINT64 last_write, UINT64, FSP_FSCTL_FILE_INFO* info) {
    return guarded([&] {
        // Only the modification time is stored; attributes and the other times are not.
        SetAttr changes;
        if (last_write != 0 && last_write >= kUnixEpochAsFileTime) {
            // Clamped: nanoseconds since 1970 in an int64 end in the year 2262.
            constexpr UINT64 kMaxTicks = static_cast<UINT64>(INT64_MAX / 100);
            const UINT64 ticks = std::min(last_write - kUnixEpochAsFileTime, kMaxTicks);
            changes.mtime_ns = static_cast<std::int64_t>(ticks) * 100;
        }
        return set_attr(impl_of(fsp), node_of(context), changes, info);
    });
}

NTSTATUS op_set_file_size(FSP_FILE_SYSTEM* fsp, PVOID context, UINT64 new_size, BOOLEAN allocation,
                          FSP_FSCTL_FILE_INFO* info) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        auto& node = node_of(context);
        SetAttr changes;
        // Allocation is not tracked; an allocation below the file size truncates it.
        if (!allocation || new_size < current(self, node).size) changes.size = new_size;
        return set_attr(self, node, changes, info);
    });
}

NTSTATUS op_can_delete(FSP_FILE_SYSTEM* fsp, PVOID context, PWSTR) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        auto& node = node_of(context);
        if (!self.fs.writable()) return STATUS_MEDIA_WRITE_PROTECTED;
        if (node.directory) {
            auto entries = self.fs.readdir(node.ino);
            if (entries && !entries->empty()) return STATUS_DIRECTORY_NOT_EMPTY;
        }
        return STATUS_SUCCESS;
    });
}

NTSTATUS op_rename(FSP_FILE_SYSTEM* fsp, PVOID context, PWSTR name, PWSTR new_name,
                   BOOLEAN replace) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        auto& node = node_of(context);
        auto [from_dir, from_leaf] = split(name);
        auto [to_dir, to_leaf] = split(new_name);
        Found from_parent;
        Found to_parent;
        if (!NT_SUCCESS(resolve(self.fs, from_dir, from_parent)) ||
            !NT_SUCCESS(resolve(self.fs, to_dir, to_parent)) ||
            to_parent.attr.type != NodeType::directory) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        auto source = find_child(self.fs, from_parent.ino, from_leaf);
        if (!source || source->ino != node.ino) return STATUS_OBJECT_NAME_NOT_FOUND;
        auto target = to_utf8(to_leaf);
        if (!windows_name_ok(to_leaf) || !target) return STATUS_OBJECT_NAME_INVALID;
        if (auto existing = find_child(self.fs, to_parent.ino, to_leaf);
            existing && existing->ino != source->ino) {
            // A name that differs only in case is the same name here.
            if (!replace) return STATUS_OBJECT_NAME_COLLISION;
            auto attr = self.fs.getattr(existing->ino);
            if (attr && attr->type == NodeType::directory) return STATUS_ACCESS_DENIED;
            target = existing->name;
        } else if (existing && existing->name == *target && from_parent.ino == to_parent.ino) {
            return STATUS_SUCCESS;
        }
        return to_ntstatus(
            self.fs.rename(from_parent.ino, source->name, to_parent.ino, *target, !replace));
    });
}

NTSTATUS op_get_security(FSP_FILE_SYSTEM* fsp, PVOID, PSECURITY_DESCRIPTOR security,
                         SIZE_T* security_size) {
    return guarded([&] { return copy_security(impl_of(fsp), security, security_size); });
}

bool add_entry(Impl& self, std::wstring_view name, const Attr& attr, PVOID buffer, ULONG length,
               PULONG transferred) {
    // FSP_FSCTL_DIR_INFO ends in the name (a flexible array member).
    alignas(FSP_FSCTL_DIR_INFO)
        UINT8 storage[sizeof(FSP_FSCTL_DIR_INFO) + (kMaxNameChars + 1) * sizeof(WCHAR)] = {};
    auto* entry = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(storage);
    const std::size_t chars = std::min<std::size_t>(name.size(), kMaxNameChars);
    entry->Size =
        static_cast<UINT16>(offsetof(FSP_FSCTL_DIR_INFO, FileNameBuf) + chars * sizeof(WCHAR));
    fill(self, attr, &entry->FileInfo);
    std::memcpy(entry->FileNameBuf, name.data(), chars * sizeof(WCHAR));
    return FspFileSystemAddDirInfo(entry, buffer, length, transferred) != FALSE;
}

NTSTATUS op_read_directory(FSP_FILE_SYSTEM* fsp, PVOID context, PWSTR, PWSTR marker, PVOID buffer,
                           ULONG length, PULONG transferred) {
    return guarded([&] {
        auto& self = impl_of(fsp);
        auto& node = node_of(context);
        if (!node.directory) return STATUS_NOT_A_DIRECTORY;
        std::lock_guard lock(node.listing_mutex);
        if (marker == nullptr || node.listing.empty()) {
            auto entries = self.fs.readdir(node.ino);
            if (!entries) return STATUS_OBJECT_NAME_NOT_FOUND;
            std::vector<std::pair<std::wstring, Ino>> listing;
            // Only the first of names that differ only in case, as find_child resolves them.
            std::unordered_set<std::wstring> seen;
            for (const auto& e : *entries) {
                auto wide = to_wide(e.name);
                if (!wide || !seen.insert(fold(*wide)).second) continue;
                if (windows_name_ok(*wide)) listing.emplace_back(std::move(*wide), e.ino);
            }
            std::sort(listing.begin(), listing.end());
            if (node.ino != kRootIno) {
                listing.insert(listing.begin(), {{L".", node.ino}, {L"..", node.parent}});
            }
            node.listing = std::move(listing);
        }
        std::size_t start = 0;
        if (marker != nullptr) {
            const std::wstring_view after(marker);
            auto it = std::find_if(node.listing.begin(), node.listing.end(),
                                   [&](const auto& e) { return e.first == after; });
            if (it != node.listing.end()) {
                start = static_cast<std::size_t>(it - node.listing.begin()) + 1;
            } else {
                // The marker's entry went away: continue with the next name in order, past the
                // "." and ".." that lead the listing.
                const auto sorted = node.listing.begin() + (node.ino == kRootIno ? 0 : 2);
                start = static_cast<std::size_t>(
                    std::upper_bound(
                        sorted, node.listing.end(), after,
                        [](std::wstring_view m, const auto& e) { return m < e.first; }) -
                    node.listing.begin());
            }
        }
        for (std::size_t i = start; i < node.listing.size(); ++i) {
            const auto& [name, ino] = node.listing[i];
            auto attr = self.fs.getattr(ino);
            if (!attr) continue;
            if (!add_entry(self, name, *attr, buffer, length, transferred)) {
                return STATUS_SUCCESS;  // full; the next call continues after the last name
            }
        }
        FspFileSystemAddDirInfo(nullptr, buffer, length, transferred);
        return STATUS_SUCCESS;
    });
}

const FSP_FILE_SYSTEM_INTERFACE kInterface = [] {
    FSP_FILE_SYSTEM_INTERFACE ops{};
    ops.GetVolumeInfo = op_get_volume_info;
    ops.GetSecurityByName = op_get_security_by_name;
    ops.Create = op_create;
    ops.Open = op_open;
    ops.Overwrite = op_overwrite;
    ops.Cleanup = op_cleanup;
    ops.Close = op_close;
    ops.Read = op_read;
    ops.Write = op_write;
    ops.Flush = op_flush;
    ops.GetFileInfo = op_get_file_info;
    ops.SetBasicInfo = op_set_basic_info;
    ops.SetFileSize = op_set_file_size;
    ops.CanDelete = op_can_delete;
    ops.Rename = op_rename;
    ops.GetSecurity = op_get_security;
    ops.ReadDirectory = op_read_directory;
    return ops;
}();

std::string describe(const std::filesystem::path& path) {
    return to_utf8(path.native()).value_or("the mountpoint");
}

std::string status_text(NTSTATUS st) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(st));
    return text;
}

// O:user G:user D:P(A;;FA;;;user): the current user owns everything and is the only one let
// in. Self-relative, as WinFsp requires.
bool make_owner_only(Impl& self) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buffer(len);
    const bool got = len != 0 && ::GetTokenInformation(token, TokenUser, buffer.data(), len, &len);
    ::CloseHandle(token);
    if (!got) return false;
    LPWSTR sid = nullptr;
    if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
        return false;
    }
    const std::wstring user(sid);
    ::LocalFree(sid);
    const std::wstring sddl = L"O:" + user + L"G:" + user + L"D:P(A;;FA;;;" + user + L")";
    return ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
               sddl.c_str(), SDDL_REVISION_1, &self.security, &self.security_size) != FALSE;
}

bool is_drive_letter(const std::wstring& point) {
    return point.size() == 2 && std::iswalpha(point[0]) && point[1] == L':';
}

// Creates the missing directories of `dir` owner-only, from the top down, so no one else can
// replace one of them (with a junction to elsewhere, say) before the mount exists. Existing
// ones are left as they are.
core::Status make_parents_private(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> missing;
    std::error_code ec;
    for (auto p = dir;
         !p.empty() && !std::filesystem::exists(std::filesystem::symlink_status(p, ec));
         p = p.parent_path()) {
        missing.push_back(p);
        if (p == p.parent_path()) break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (auto st = core::make_private_dir(*it); !st) return st;
    }
    return {};
}

// True if `path` is `root` (which ends in a separator) or lies below it, ignoring case.
bool lies_inside(const std::filesystem::path& path, const std::wstring& root) {
    std::error_code ec;
    auto full = std::filesystem::absolute(path, ec);
    if (ec) return false;
    full = full.lexically_normal();
    full.make_preferred();
    const std::wstring candidate = fold(full.native() + L"\\");
    const std::wstring prefix = fold(root);
    return candidate.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

WinFspMount::WinFspMount(Private, std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WinFspMount::~WinFspMount() {
    unmount();
}

const std::filesystem::path& WinFspMount::mountpoint() const noexcept {
    return impl_->mountpoint;
}

core::Result<std::unique_ptr<WinFspMount>> WinFspMount::mount(
    Filesystem& fs, const std::filesystem::path& mountpoint, const WinFspMountOptions& options) {
    // winfsp-x64.dll is delay-loaded from the WinFsp installation; without it nothing else here
    // may be called.
    if (!NT_SUCCESS(FspLoad(nullptr))) {
        return Error{Errc::not_found,
                     "WinFsp is not installed; get it from https://winfsp.dev or run with "
                     "--no-mount"};
    }
    auto impl = std::make_unique<Impl>(fs, mountpoint, options);
    if (auto label = to_wide(options.volume_name)) impl->label = std::move(*label);
    if (!make_owner_only(*impl)) {
        return Error{Errc::io, "cannot build the mount's security descriptor"};
    }

    std::wstring point = mountpoint.native();
    std::wstring root = point + L"\\";
    if (!is_drive_letter(point)) {
        std::error_code ec;
        auto dir = std::filesystem::absolute(mountpoint, ec);
        if (ec) return Error{Errc::invalid_argument, "invalid mountpoint " + describe(mountpoint)};
        dir = dir.lexically_normal();
        dir.make_preferred();
        if (!dir.has_filename()) dir = dir.parent_path();
        // WinFsp creates the directory itself and removes it on unmount. Whatever is already
        // there (a directory, a junction, a file) belongs to the user and is left alone.
        if (std::filesystem::exists(std::filesystem::symlink_status(dir, ec))) {
            return Error{Errc::already_exists,
                         "cannot mount at " + describe(dir) +
                             ": it exists; the mountpoint must be a free drive letter or a path "
                             "that does not exist yet"};
        }
        if (auto st = make_parents_private(dir.parent_path()); !st) return st.error();
        point = dir.native();
        root = point + L"\\";
    }
    if (!options.data_dir.empty() && lies_inside(options.data_dir, root)) {
        return Error{Errc::invalid_argument, "the data directory " + describe(options.data_dir) +
                                                 " must not be inside the mount"};
    }

    FSP_FSCTL_VOLUME_PARAMS params{};
    params.Version = sizeof(params);
    params.SectorSize = 4096;
    params.SectorsPerAllocationUnit = 1;
    params.MaxComponentLength = kMaxNameChars;
    params.VolumeCreationTime = impl->mounted_at;
    params.VolumeSerialNumber = static_cast<UINT32>(impl->mounted_at / (10000 * 1000));
    // Not INFINITE: with DisableCache on every open (see opened()), the system cache stays off.
    params.FileInfoTimeout = static_cast<UINT32>(options.info_timeout.count());
    params.CaseSensitiveSearch = 0;
    params.CasePreservedNames = 1;
    params.UnicodeOnDisk = 1;
    params.PersistentAcls = 1;
    params.ReadOnlyVolume = fs.writable() ? 0 : 1;
    params.UmFileContextIsUserContext2 = 1;
    wcscpy_s(params.FileSystemName, sizeof(params.FileSystemName) / sizeof(WCHAR), L"GhostDrive");

    std::wstring device = L"" FSP_FSCTL_DISK_DEVICE_NAME;
    NTSTATUS st = FspFileSystemCreate(device.data(), &params, &kInterface, &impl->fsp);
    if (!NT_SUCCESS(st)) return Error{Errc::io, "FspFileSystemCreate failed: " + status_text(st)};
    impl->fsp->UserContext = impl.get();
    FspFileSystemSetDebugLog(impl->fsp, options.debug ? ~UINT32(0) : 0);

    st = FspFileSystemSetMountPointEx(impl->fsp, point.data(), impl->security);
    if (NT_SUCCESS(st)) st = FspFileSystemStartDispatcher(impl->fsp, 0);
    if (!NT_SUCCESS(st)) {
        FspFileSystemDelete(impl->fsp);
        impl->fsp = nullptr;
        const std::string why = st == STATUS_OBJECT_NAME_COLLISION || st == STATUS_ACCESS_DENIED
                                    ? " (it must be a free drive letter or a path that does "
                                      "not exist yet)"
                                    : "";
        return Error{Errc::io,
                     "cannot mount at " + describe(mountpoint) + ": " + status_text(st) + why};
    }
    impl->mounted = true;
    return std::make_unique<WinFspMount>(Private{}, std::move(impl));
}

void WinFspMount::unmount() {
    if (!impl_ || !impl_->mounted) return;
    impl_->mounted = false;
    // Stopping the dispatcher cancels the requests still pending (FSP_FSCTL_STOP) and unmaps
    // their buffers, so late replies must stop touching them first.
    {
        std::unique_lock gate(impl_->replies);
        impl_->replies_open = false;
    }
    FspFileSystemStopDispatcher(impl_->fsp);
    {
        std::unique_lock lock(impl_->pending_mutex);
        impl_->pending_done.wait(lock, [&] { return impl_->pending == 0; });
    }
    // WinFsp sends no Close for files still open now. Their writes are committed here rather
    // than lost, and their handles released.
    std::vector<std::unique_ptr<Node>> left;
    {
        std::lock_guard lock(impl_->nodes_mutex);
        for (void* p : impl_->nodes) left.emplace_back(static_cast<Node*>(p));
        impl_->nodes.clear();
    }
    for (auto& node : left) {
        if (!node->handle) continue;
        if (node->write) {
            (void)wait_for<core::Status>(
                [&](auto done) { impl_->fs.flush(*node->handle, std::move(done)); });
        }
        impl_->fs.release(*node->handle);
    }
    FspFileSystemDelete(impl_->fsp);  // also removes the mountpoint
    impl_->fsp = nullptr;
}

}  // namespace ghostdrive::vfs
