// libfuse3 low-level adapter. FUSE_USE_VERSION 35 keeps to API that Linux libfuse 3.5+ and
// FUSE-T (libfuse 3.19 fork) share, including the fuse_loop_config form of the session loop.
#define FUSE_USE_VERSION 35

#include "ghostdrive/vfs/fuse_mount.hpp"

#include <fcntl.h>
#include <fuse_lowlevel.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;

struct FuseMount::Impl {
    Impl(Filesystem& fs, std::filesystem::path mountpoint, FuseMountOptions options)
        : fs(fs), mountpoint(std::move(mountpoint)), options(std::move(options)) {}

    Filesystem& fs;
    std::filesystem::path mountpoint;
    FuseMountOptions options;
    fuse_session* session = nullptr;
    std::thread loop;
    bool mounted = false;

    // Directory listings are snapshotted at opendir so offsets stay stable while reading.
    std::mutex dirs_mutex;
    std::unordered_map<std::uint64_t, std::vector<DirEntry>> dirs;
    std::uint64_t next_dir = 1;
};

namespace {

FuseMount::Impl& impl_of(fuse_req_t req) {
    return *static_cast<FuseMount::Impl*>(fuse_req_userdata(req));
}

struct stat to_stat(const Attr& attr) {
    struct stat st{};
    st.st_ino = static_cast<ino_t>(attr.ino);
    st.st_mode =
        static_cast<mode_t>((attr.type == NodeType::directory ? S_IFDIR : S_IFREG) | attr.perm);
    st.st_nlink = static_cast<nlink_t>(attr.nlink);
    st.st_size = static_cast<off_t>(attr.size);
    st.st_blksize = 4096;
    st.st_blocks = static_cast<blkcnt_t>((attr.size + 511) / 512);
    st.st_uid = ::getuid();
    st.st_gid = ::getgid();
    timespec mtime{static_cast<time_t>(attr.mtime_ns / 1'000'000'000),
                   static_cast<long>(attr.mtime_ns % 1'000'000'000)};
#ifdef __APPLE__
    st.st_mtimespec = mtime;
    st.st_ctimespec = mtime;
    st.st_atimespec = mtime;
#else
    st.st_mtim = mtime;
    st.st_ctim = mtime;
    st.st_atim = mtime;
#endif
    return st;
}

void op_lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
    auto& self = impl_of(req);
    auto ino = self.fs.lookup(parent, name);
    auto attr = ino ? self.fs.getattr(*ino) : std::nullopt;
    if (!attr) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    fuse_entry_param entry{};
    entry.ino = attr->ino;
    entry.generation = 0;  // inode numbers are never reused
    entry.attr = to_stat(*attr);
    entry.attr_timeout = self.options.attr_timeout_s;
    entry.entry_timeout = self.options.entry_timeout_s;
    fuse_reply_entry(req, &entry);
}

void op_forget(fuse_req_t req, fuse_ino_t, uint64_t) {
    // Nodes are owned by the index, not by kernel lookup counts.
    fuse_reply_none(req);
}

void op_getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info*) {
    auto& self = impl_of(req);
    auto attr = self.fs.getattr(ino);
    if (!attr) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    auto st = to_stat(*attr);
    fuse_reply_attr(req, &st, self.options.attr_timeout_s);
}

void op_opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    auto& self = impl_of(req);
    auto entries = self.fs.readdir(ino);
    if (!entries) {
        fuse_reply_err(req, self.fs.getattr(ino) ? ENOTDIR : ENOENT);
        return;
    }
    std::vector<DirEntry> listing;
    listing.reserve(entries->size() + 2);
    listing.push_back(DirEntry{".", ino, NodeType::directory});
    listing.push_back(DirEntry{"..", FUSE_ROOT_ID, NodeType::directory});
    for (auto& e : *entries) listing.push_back(std::move(e));
    {
        std::lock_guard lock(self.dirs_mutex);
        fi->fh = self.next_dir++;
        self.dirs.emplace(fi->fh, std::move(listing));
    }
    fuse_reply_open(req, fi);
}

void op_readdir(fuse_req_t req, fuse_ino_t, size_t size, off_t off, fuse_file_info* fi) {
    auto& self = impl_of(req);
    std::vector<char> buffer(size);
    std::size_t used = 0;
    {
        std::lock_guard lock(self.dirs_mutex);
        auto it = self.dirs.find(fi->fh);
        if (it == self.dirs.end()) {
            fuse_reply_err(req, EBADF);
            return;
        }
        const auto& listing = it->second;
        for (auto i = static_cast<std::size_t>(std::max<off_t>(off, 0)); i < listing.size(); ++i) {
            struct stat st{};
            st.st_ino = static_cast<ino_t>(listing[i].ino);
            st.st_mode = listing[i].type == NodeType::directory ? S_IFDIR : S_IFREG;
            std::size_t need =
                fuse_add_direntry(req, buffer.data() + used, size - used, listing[i].name.c_str(),
                                  &st, static_cast<off_t>(i + 1));
            if (need > size - used) break;  // the entry did not fit; the kernel asks again
            used += need;
        }
    }
    fuse_reply_buf(req, buffer.data(), used);
}

void op_releasedir(fuse_req_t req, fuse_ino_t, fuse_file_info* fi) {
    auto& self = impl_of(req);
    {
        std::lock_guard lock(self.dirs_mutex);
        self.dirs.erase(fi->fh);
    }
    fuse_reply_err(req, 0);
}

void op_open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    auto& self = impl_of(req);
    if ((fi->flags & O_ACCMODE) != O_RDONLY || (fi->flags & O_TRUNC) != 0) {
        fuse_reply_err(req, EROFS);
        return;
    }
    auto handle = self.fs.open(ino);
    if (!handle) {
        fuse_reply_err(req, self.fs.getattr(ino) ? EISDIR : ENOENT);
        return;
    }
    fi->fh = handle.value();
    // The open pins a version; cached pages from an older version must not be reused.
    fi->keep_cache = 0;
    fuse_reply_open(req, fi);
}

void op_read(fuse_req_t req, fuse_ino_t, size_t size, off_t off, fuse_file_info* fi) {
    auto& self = impl_of(req);
    if (off < 0) {
        fuse_reply_err(req, EINVAL);
        return;
    }
    // Returns without replying; the callback replies from the Filesystem's executor, so this
    // FUSE thread is free while the chunk is fetched.
    self.fs.read(
        fi->fh, static_cast<std::uint64_t>(off), size, [req](core::Result<ReadReply> result) {
            if (!result) {
                // Any failure to produce the bytes is an I/O error to the reader.
                fuse_reply_err(req, result.error().code == Errc::invalid_argument ? EBADF : EIO);
                return;
            }
            const auto& slices = result.value().slices;
            std::vector<iovec> iov(slices.size());
            for (std::size_t i = 0; i < slices.size(); ++i) {
                // fuse_reply_iov only reads through this pointer.
                iov[i].iov_base = const_cast<std::byte*>(slices[i].data());
                iov[i].iov_len = slices[i].size();
            }
            // COPY: the kernel copies the reply out of the chunk buffers (C8).
            fuse_reply_iov(req, iov.data(), static_cast<int>(iov.size()));
        });
}

void op_release(fuse_req_t req, fuse_ino_t, fuse_file_info* fi) {
    impl_of(req).fs.release(fi->fh);
    fuse_reply_err(req, 0);
}

void op_statfs(fuse_req_t req, fuse_ino_t) {
    struct statvfs st{};
    st.f_bsize = 4096;
    st.f_frsize = 4096;
    st.f_namemax = 255;
    fuse_reply_statfs(req, &st);
}

// FUSE-T mounts over NFS in the background, so the mount exists some time after
// fuse_session_mount returns; Linux mounts synchronously and passes at once. A mounted directory
// reports a different device than its parent.
bool wait_until_mounted(const std::filesystem::path& mountpoint, std::chrono::milliseconds limit) {
    struct stat parent{};
    if (::stat(mountpoint.parent_path().c_str(), &parent) != 0) return false;
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        struct stat st{};
        if (::stat(mountpoint.c_str(), &st) == 0 && st.st_dev != parent.st_dev) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

const fuse_lowlevel_ops kOps = [] {
    fuse_lowlevel_ops ops{};
    ops.lookup = op_lookup;
    ops.forget = op_forget;
    ops.getattr = op_getattr;
    ops.open = op_open;
    ops.read = op_read;
    ops.release = op_release;
    ops.opendir = op_opendir;
    ops.readdir = op_readdir;
    ops.releasedir = op_releasedir;
    ops.statfs = op_statfs;
    return ops;
}();

}  // namespace

FuseMount::FuseMount(Private, std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FuseMount::~FuseMount() {
    unmount();
}

const std::filesystem::path& FuseMount::mountpoint() const noexcept {
    return impl_->mountpoint;
}

core::Result<std::unique_ptr<FuseMount>> FuseMount::mount(Filesystem& fs,
                                                          const std::filesystem::path& mountpoint,
                                                          const FuseMountOptions& options) {
    auto impl = std::make_unique<Impl>(fs, mountpoint, options);
    // As fuse_set_signal_handlers does: FUSE-T talks to its NFS server over a socket, and a write
    // to it after the server side closes must fail with EPIPE instead of killing the process.
    std::signal(SIGPIPE, SIG_IGN);

    std::vector<std::string> args{"ghostdrive", "-o", "ro", "-o", "fsname=ghostdrive"};
#ifdef __APPLE__
    args.insert(args.end(), {"-o", "volname=" + options.volume_name});
#endif
    if (options.debug) args.emplace_back("-d");
    for (const auto& extra : options.extra_options) args.insert(args.end(), {"-o", extra});
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    fuse_args fargs = FUSE_ARGS_INIT(static_cast<int>(argv.size()), argv.data());

    impl->session = fuse_session_new(&fargs, &kOps, sizeof(kOps), impl.get());
    fuse_opt_free_args(&fargs);
    if (impl->session == nullptr) return Error{Errc::io, "fuse_session_new failed"};

    if (fuse_session_mount(impl->session, mountpoint.c_str()) != 0) {
        fuse_session_destroy(impl->session);
        impl->session = nullptr;
        return Error{Errc::io, "fuse_session_mount failed for " + mountpoint.string()};
    }
    impl->mounted = true;
    impl->loop = std::thread([session = impl->session] {
        fuse_loop_config config{};
        config.clone_fd = 0;
        config.max_idle_threads = 10;
        fuse_session_loop_mt(session, &config);
    });
    auto mount = std::make_unique<FuseMount>(Private{}, std::move(impl));
    if (!wait_until_mounted(mountpoint, std::chrono::seconds(10))) {
        mount->unmount();
        return Error{Errc::timeout, "mount did not appear at " + mountpoint.string()};
    }
    return mount;
}

void FuseMount::unmount() {
    if (!impl_ || !impl_->mounted) return;
    impl_->mounted = false;
    fuse_session_exit(impl_->session);
    // Unmounting makes the kernel side end the session, which returns the loop.
    fuse_session_unmount(impl_->session);
    if (impl_->loop.joinable()) impl_->loop.join();
    fuse_session_destroy(impl_->session);
    impl_->session = nullptr;
}

}  // namespace ghostdrive::vfs
