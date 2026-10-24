#include "ghostdrive/vfs/filesystem.hpp"

#include <algorithm>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <cerrno>
#include <string>

#include "write_handle.hpp"

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;
using core::Status;

namespace {

std::int64_t wall_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool valid_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

std::string join(const std::string& dir, std::string_view name) {
    return dir.empty() ? std::string(name) : dir + "/" + std::string(name);
}

bool is_under(std::string_view path, std::string_view dir) {
    return path == dir || (path.size() > dir.size() && path.substr(0, dir.size()) == dir &&
                           path[dir.size()] == '/');
}

}  // namespace

// Exactly one of `view` (read-only) or `writer` is set. The queue is touched only on the io
// executor; it keeps one handle's operations in order.
struct Filesystem::Handle {
    Ino ino = 0;
    std::shared_ptr<FileView> view;
    std::shared_ptr<WriteHandle> writer;
    std::deque<Op> queue;
    bool running = false;
};

Filesystem::Filesystem(asio::any_io_executor io, Tree& tree, Fetcher& fetcher,
                       FilesystemOptions options, std::optional<WriteSupport> writes)
    : io_(std::move(io)),
      tree_(tree),
      fetcher_(fetcher),
      options_(options),
      writes_(std::move(writes)) {
    if (writes_) {
        std::error_code ec;
        std::filesystem::create_directories(writes_->staging_dir, ec);
    }
}

Filesystem::~Filesystem() = default;

Status Filesystem::require_writable() const {
    if (!writes_) return Error{Errc::read_only, "the filesystem is mounted read-only"};
    return {};
}

std::optional<Attr> Filesystem::getattr(Ino ino) const {
    auto attr = tree_.getattr(ino);
    if (attr && attr->type == NodeType::file) {
        auto writers = writers_for(ino);
        if (!writers.empty()) attr->size = writers.front()->writer->size();
    }
    return attr;
}

core::Result<std::string> Filesystem::child_path(Ino parent, std::string_view name) const {
    if (!valid_name(name)) return Error{Errc::invalid_argument, "invalid file name"};
    auto attr = tree_.getattr(parent);
    if (!attr) return Error{Errc::not_found, "no such directory"};
    if (attr->type != NodeType::directory) return Error{Errc::not_directory, "not a directory"};
    auto dir = tree_.path_of(parent);
    if (!dir) return Error{Errc::not_found, "no such directory"};
    auto path = join(*dir, name);
    if (!core::is_valid_drive_path(path)) return Error{Errc::invalid_argument, "invalid path"};
    return path;
}

std::shared_ptr<Filesystem::Handle> Filesystem::find(std::uint64_t handle) const {
    std::lock_guard lock(mutex_);
    auto it = handles_.find(handle);
    return it == handles_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Filesystem::Handle>> Filesystem::writers_for(Ino ino) const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Handle>> out;
    auto it = writers_.find(ino);
    if (it == writers_.end()) return out;
    for (auto w = it->second.rbegin(); w != it->second.rend(); ++w) {
        if (auto h = w->lock()) out.push_back(std::move(h));
    }
    return out;
}

void Filesystem::enqueue(std::shared_ptr<Handle> handle, Op op) {
    asio::post(io_, [io = io_, handle = std::move(handle), op = std::move(op)]() mutable {
        handle->queue.push_back(std::move(op));
        if (handle->running) return;
        handle->running = true;
        asio::co_spawn(
            io,
            [handle]() -> asio::awaitable<void> {
                while (!handle->queue.empty()) {
                    auto next = std::move(handle->queue.front());
                    handle->queue.pop_front();
                    co_await next();
                }
                handle->running = false;
            },
            asio::detached);
    });
}

core::Result<std::shared_ptr<Filesystem::Handle>> Filesystem::make_writer(
    Ino ino, std::string path, std::optional<core::FileRecord> base, std::uint32_t perm,
    bool truncate) {
    std::uint64_t staging_id;
    {
        std::lock_guard lock(mutex_);
        staging_id = next_staging_++;
    }
    WriteHandle::Context context{
        .store = writes_->store,
        .index = writes_->index,
        .clock = writes_->clock,
        .fetcher = fetcher_,
        .disk = writes_->disk,
        .mutation_mutex = mutation_mutex_,
        .after_commit = [this] { refresh_locked(); },
    };
    auto writer =
        WriteHandle::open(std::move(context), std::move(path), std::move(base), perm,
                          writes_->staging_dir / ("w" + std::to_string(staging_id)), truncate);
    if (!writer) return writer.error();
    auto handle = std::make_shared<Handle>();
    handle->ino = ino;
    handle->writer = std::move(writer).value();
    std::lock_guard lock(mutex_);
    auto& list = writers_[ino];
    std::erase_if(list, [](const auto& w) { return w.expired(); });
    list.push_back(handle);
    return handle;
}

core::Result<std::uint64_t> Filesystem::open(Ino ino, bool write, bool truncate) {
    auto attr = tree_.getattr(ino);
    if (!attr) return Error{Errc::not_found, "no such file"};
    if (attr->type == NodeType::directory) return Error{Errc::is_directory, "is a directory"};
    auto record = tree_.file_record(ino);
    if (!record) return Error{Errc::not_found, "no such file"};

    std::shared_ptr<Handle> handle;
    if (write) {
        if (auto st = require_writable(); !st) return st.error();
        auto made = make_writer(ino, record->path, *record, record->mode & 07777, truncate);
        if (!made) return made.error();
        handle = std::move(made).value();
    } else {
        handle = std::make_shared<Handle>();
        handle->ino = ino;
        handle->view =
            std::make_shared<FileView>(std::move(*record), fetcher_, options_.max_readahead_chunks);
    }
    std::lock_guard lock(mutex_);
    std::uint64_t id = next_handle_++;
    handles_.emplace(id, std::move(handle));
    return id;
}

Status Filesystem::commit_record_locked(core::FileRecord record) {
    auto committed = writes_->index.commit_local(std::move(record), writes_->clock);
    if (!committed) return committed.error();
    refresh_locked();
    return {};
}

void Filesystem::refresh_locked() {
    if (writes_) (void)tree_.refresh(writes_->index);
}

core::Result<std::size_t> Filesystem::refresh(const core::MetadataIndex& index) {
    std::lock_guard lock(mutation_mutex_);
    return tree_.refresh(index);
}

core::Result<std::pair<Attr, std::uint64_t>> Filesystem::create(Ino parent, std::string_view name,
                                                                std::uint32_t perm) {
    if (auto st = require_writable(); !st) return st.error();
    auto path = child_path(parent, name);
    if (!path) return path.error();
    Ino ino;
    {
        std::lock_guard lock(mutation_mutex_);
        if (tree_.resolve(path.value())) return Error{Errc::already_exists, "file exists"};
        core::FileRecord record;
        record.path = path.value();
        record.mode = perm & 07777;
        record.mtime_ns = wall_clock_ns();
        if (auto st = commit_record_locked(std::move(record)); !st) return st.error();
        auto resolved = tree_.resolve(path.value());
        if (!resolved) return Error{Errc::io, "created file did not appear"};
        ino = *resolved;
    }
    auto handle = open(ino, true, false);
    if (!handle) return handle.error();
    auto attr = getattr(ino);
    if (!attr) return Error{Errc::not_found, "created file vanished"};
    return std::pair{*attr, handle.value()};
}

core::Result<Attr> Filesystem::mkdir(Ino parent, std::string_view name, std::uint32_t perm) {
    if (auto st = require_writable(); !st) return st.error();
    auto path = child_path(parent, name);
    if (!path) return path.error();
    std::lock_guard lock(mutation_mutex_);
    if (tree_.resolve(path.value())) return Error{Errc::already_exists, "file exists"};
    core::FileRecord record;
    record.path = path.value();
    record.mode = kModeDirectory | (perm & 07777);
    record.mtime_ns = wall_clock_ns();
    if (auto st = commit_record_locked(std::move(record)); !st) return st.error();
    auto ino = tree_.resolve(path.value());
    auto attr = ino ? tree_.getattr(*ino) : std::nullopt;
    if (!attr) return Error{Errc::io, "created directory did not appear"};
    return *attr;
}

Status Filesystem::unlink(Ino parent, std::string_view name) {
    if (auto st = require_writable(); !st) return st;
    auto path = child_path(parent, name);
    if (!path) return path.error();
    std::lock_guard lock(mutation_mutex_);
    auto ino = tree_.resolve(path.value());
    if (!ino) return Error{Errc::not_found, "no such file"};
    if (tree_.getattr(*ino)->type == NodeType::directory) {
        return Error{Errc::is_directory, "is a directory"};
    }
    auto deleted = writes_->index.delete_local(path.value(), writes_->clock);
    if (!deleted) return deleted.error();
    // POSIX: open descriptors keep working, but their writes no longer reach the drive.
    for (auto& h : writers_for(*ino)) h->writer->orphan();
    refresh_locked();
    return {};
}

Status Filesystem::rmdir(Ino parent, std::string_view name) {
    if (auto st = require_writable(); !st) return st;
    auto path = child_path(parent, name);
    if (!path) return path.error();
    std::lock_guard lock(mutation_mutex_);
    auto ino = tree_.resolve(path.value());
    if (!ino) return Error{Errc::not_found, "no such directory"};
    if (tree_.getattr(*ino)->type != NodeType::directory) {
        return Error{Errc::not_directory, "not a directory"};
    }
    if (!tree_.readdir(*ino)->empty()) return Error{Errc::not_empty, "directory not empty"};
    if (tree_.file_record(*ino)) {
        auto deleted = writes_->index.delete_local(path.value(), writes_->clock);
        if (!deleted) return deleted.error();
    }
    refresh_locked();
    return {};
}

Status Filesystem::rename(Ino parent, std::string_view name, Ino new_parent,
                          std::string_view new_name, bool no_replace) {
    if (auto st = require_writable(); !st) return st;
    auto from = child_path(parent, name);
    if (!from) return from.error();
    auto to = child_path(new_parent, new_name);
    if (!to) return to.error();
    std::lock_guard lock(mutation_mutex_);

    auto src = tree_.resolve(from.value());
    if (!src) return Error{Errc::not_found, "no such file"};
    if (from.value() == to.value()) return {};
    bool src_dir = tree_.getattr(*src)->type == NodeType::directory;
    if (src_dir && is_under(to.value(), from.value())) {
        return Error{Errc::invalid_argument, "cannot move a directory into itself"};
    }

    auto& index = writes_->index;
    auto& clock = writes_->clock;
    if (auto dst = tree_.resolve(to.value())) {
        if (no_replace) return Error{Errc::already_exists, "target exists"};
        bool dst_dir = tree_.getattr(*dst)->type == NodeType::directory;
        if (!src_dir && dst_dir) return Error{Errc::is_directory, "target is a directory"};
        if (src_dir && !dst_dir) return Error{Errc::not_directory, "target is not a directory"};
        if (dst_dir) {
            if (!tree_.readdir(*dst)->empty()) return Error{Errc::not_empty, "target not empty"};
            if (tree_.file_record(*dst)) {
                if (auto d = index.delete_local(to.value(), clock); !d) return d.error();
            }
            refresh_locked();  // drops the empty target so the move can take its place
        } else {
            for (auto& h : writers_for(*dst)) h->writer->orphan();
        }
    }

    // Not atomic across records: a crash midway leaves both copies, which replicate like any
    // other files and lose nothing.
    auto paths = tree_.record_paths_under(*src);
    for (const auto& path : paths) {
        auto record = index.get(path);
        if (!record) return record.error();
        if (!record.value()) continue;
        auto moved = std::move(*record.value());
        moved.path = to.value() + path.substr(from.value().size());
        if (auto c = index.commit_local(std::move(moved), clock); !c) return c.error();
    }
    for (const auto& path : paths) {
        auto d = index.delete_local(path, clock);
        if (!d && d.error().code != Errc::not_found) return d.error();
    }
    // Move the nodes before the refresh applies the new records, so inodes carry over.
    tree_.move(from.value(), to.value());
    {
        std::lock_guard handles_lock(mutex_);
        for (auto& [id, h] : handles_) {
            if (!h->writer || !is_under(h->writer->path(), from.value())) continue;
            h->writer->set_path(to.value() + h->writer->path().substr(from.value().size()));
        }
    }
    refresh_locked();
    return {};
}

void Filesystem::read(std::uint64_t handle, std::uint64_t offset, std::size_t size,
                      ReadCallback done) {
    auto h = find(handle);
    if (!h) {
        asio::post(io_, [done = std::move(done)] {
            done(Error{Errc::invalid_argument, "unknown file handle"});
        });
        return;
    }
    if (h->writer) {
        enqueue(h, [h, offset, size, done = std::move(done)]() -> asio::awaitable<void> {
            done(co_await h->writer->read(offset, size));
        });
        return;
    }

    // Whichever finishes first, the read or the deadline, answers; the other is ignored.
    struct State {
        ReadCallback done;
        asio::steady_timer deadline;
        bool answered = false;
    };
    auto state = std::make_shared<State>(State{std::move(done), asio::steady_timer(io_), false});
    state->deadline.expires_after(options_.request_timeout);
    state->deadline.async_wait([state](const std::error_code& ec) {
        if (ec || state->answered) return;
        state->answered = true;
        state->done(Error{Errc::timeout, "read timed out"});
    });

    // The view lives in the coroutine frame, so a release() during the read is safe.
    asio::co_spawn(
        io_,
        [view = h->view, state, offset, size]() -> asio::awaitable<void> {
            auto result = co_await view->read(offset, size);
            if (state->answered) co_return;
            state->answered = true;
            state->deadline.cancel();
            state->done(std::move(result));
        },
        asio::detached);
}

void Filesystem::write(std::uint64_t handle, std::uint64_t offset, core::Bytes data,
                       StatusCallback done) {
    auto h = find(handle);
    if (!h || !h->writer) {
        asio::post(io_, [done = std::move(done)] {
            done(Error{Errc::invalid_argument, "file is not open for writing"});
        });
        return;
    }
    enqueue(h,
            [h, offset, data = std::move(data),
             done = std::move(done)]() mutable -> asio::awaitable<void> {
                done(co_await h->writer->write(offset, std::move(data)));
            });
}

void Filesystem::flush(std::uint64_t handle, StatusCallback done) {
    auto h = find(handle);
    if (!h || !h->writer) {
        asio::post(io_, [done = std::move(done)] { done(Status{}); });
        return;
    }
    enqueue(h, [h, done = std::move(done)]() -> asio::awaitable<void> {
        done(co_await h->writer->commit());
    });
}

void Filesystem::setattr(Ino ino, SetAttr changes, std::optional<std::uint64_t> handle,
                         AttrCallback done) {
    auto reply = [this, ino](AttrCallback& cb, Status st) {
        if (!st) {
            cb(st.error());
            return;
        }
        auto attr = getattr(ino);
        if (!attr) {
            cb(Error{Errc::not_found, "no such file"});
            return;
        }
        cb(*attr);
    };
    bool changes_anything = changes.size || changes.perm || changes.mtime_ns;
    if (!changes_anything) {
        asio::post(io_, [reply, done = std::move(done)]() mutable { reply(done, Status{}); });
        return;
    }
    if (auto st = require_writable(); !st) {
        asio::post(io_, [done = std::move(done), st] { done(st.error()); });
        return;
    }
    auto attr = tree_.getattr(ino);
    if (!attr) {
        asio::post(io_, [done = std::move(done)] { done(Error{Errc::not_found, "no such file"}); });
        return;
    }
    if (changes.size && attr->type == NodeType::directory) {
        asio::post(io_,
                   [done = std::move(done)] { done(Error{Errc::is_directory, "is a directory"}); });
        return;
    }

    // Files with an open writer (or a size change, which needs one) go through a writer so the
    // change lands in order with pending writes.
    std::shared_ptr<Handle> writer;
    bool through_caller = false;
    if (attr->type == NodeType::file) {
        if (handle) {
            auto h = find(*handle);
            if (h && h->writer) {
                writer = h;
                through_caller = true;
            }
        }
        if (!writer) {
            auto open_writers = writers_for(ino);
            if (!open_writers.empty()) writer = open_writers.front();
        }
        if (!writer && changes.size) {
            auto path = tree_.path_of(ino);
            auto made =
                path
                    ? make_writer(ino, *path, tree_.file_record(ino), attr->perm, false)
                    : core::Result<std::shared_ptr<Handle>>(Error{Errc::not_found, "no such file"});
            if (!made) {
                asio::post(io_, [done = std::move(done), e = made.error()] { done(e); });
                return;
            }
            writer = std::move(made).value();
        }
    }

    if (writer) {
        bool commit_now = !through_caller;
        enqueue(writer,
                [writer, changes, commit_now, reply,
                 done = std::move(done)]() mutable -> asio::awaitable<void> {
                    Status st;
                    if (changes.size) st = co_await writer->writer->truncate(*changes.size);
                    if (st && changes.perm) writer->writer->set_mode(*changes.perm);
                    if (st && changes.mtime_ns) writer->writer->set_mtime(*changes.mtime_ns);
                    // A change made by path (truncate(2), chmod, touch) takes effect now; one made
                    // through this descriptor commits with its writes.
                    if (st && commit_now) st = co_await writer->writer->commit();
                    reply(done, st);
                });
        return;
    }

    // Metadata-only change with no writer: rewrite the record directly.
    Status st;
    {
        std::lock_guard lock(mutation_mutex_);
        auto path = tree_.path_of(ino);
        auto record = path ? writes_->index.get(*path)
                           : core::Result<std::optional<core::FileRecord>>(
                                 Error{Errc::not_found, "no such file"});
        if (!record) {
            st = record.error();
        } else {
            core::FileRecord updated;
            if (record.value() && !record.value()->deleted) {
                updated = std::move(*record.value());
            } else if (attr->type == NodeType::directory) {
                // An implied directory gets a record of its own to carry the change.
                updated.path = *path;
                updated.mode = kModeDirectory | attr->perm;
                updated.mtime_ns = attr->mtime_ns;
            } else {
                st = Error{Errc::not_found, "no such file"};
            }
            if (st) {
                if (changes.perm)
                    updated.mode = (updated.mode & kModeTypeMask) | (*changes.perm & 07777);
                if (changes.mtime_ns) updated.mtime_ns = *changes.mtime_ns;
                st = commit_record_locked(std::move(updated));
            }
        }
    }
    asio::post(io_, [reply, st, done = std::move(done)]() mutable { reply(done, st); });
}

void Filesystem::release(std::uint64_t handle) {
    std::shared_ptr<Handle> h;
    {
        std::lock_guard lock(mutex_);
        auto it = handles_.find(handle);
        if (it == handles_.end()) return;
        h = std::move(it->second);
        handles_.erase(it);
    }
    if (h->writer) {
        // Nothing can report a failure from release; flush already reported it to close().
        enqueue(h, [h]() -> asio::awaitable<void> { (void)co_await h->writer->commit(); });
    }
}

std::size_t Filesystem::open_handles() const {
    std::lock_guard lock(mutex_);
    return handles_.size();
}

int to_errno(const core::Error& error) noexcept {
    switch (error.code) {
        case Errc::not_found:
            return ENOENT;
        case Errc::already_exists:
            return EEXIST;
        case Errc::invalid_argument:
            return EINVAL;
        case Errc::permission:
            return EACCES;
        case Errc::timeout:
            return ETIMEDOUT;
        case Errc::closed:
            return ENOTCONN;
        case Errc::not_empty:
            return ENOTEMPTY;
        case Errc::is_directory:
            return EISDIR;
        case Errc::not_directory:
            return ENOTDIR;
        case Errc::read_only:
            return EROFS;
        case Errc::io:
        case Errc::corrupt:
        case Errc::crypto:
        case Errc::database:
        case Errc::protocol:
            return EIO;
    }
    return EIO;
}

}  // namespace ghostdrive::vfs
