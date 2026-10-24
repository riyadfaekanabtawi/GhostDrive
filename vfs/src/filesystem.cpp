#include "ghostdrive/vfs/filesystem.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <cerrno>

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;

Filesystem::Filesystem(asio::any_io_executor io, Tree& tree, Fetcher& fetcher,
                       FilesystemOptions options)
    : io_(std::move(io)), tree_(tree), fetcher_(fetcher), options_(options) {}

core::Result<std::uint64_t> Filesystem::open(Ino ino) {
    auto record = tree_.file_record(ino);
    if (!record) return Error{Errc::not_found, "no such file"};
    auto view =
        std::make_shared<FileView>(std::move(*record), fetcher_, options_.max_readahead_chunks);
    std::lock_guard lock(mutex_);
    std::uint64_t handle = next_handle_++;
    handles_.emplace(handle, std::move(view));
    return handle;
}

void Filesystem::release(std::uint64_t handle) {
    std::lock_guard lock(mutex_);
    handles_.erase(handle);
}

std::size_t Filesystem::open_handles() const {
    std::lock_guard lock(mutex_);
    return handles_.size();
}

void Filesystem::read(std::uint64_t handle, std::uint64_t offset, std::size_t size,
                      ReadCallback done) {
    std::shared_ptr<FileView> view;
    {
        std::lock_guard lock(mutex_);
        if (auto it = handles_.find(handle); it != handles_.end()) view = it->second;
    }
    if (!view) {
        asio::post(io_, [done = std::move(done)] {
            done(Error{Errc::invalid_argument, "unknown file handle"});
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
        [view, state, offset, size]() -> asio::awaitable<void> {
            auto result = co_await view->read(offset, size);
            if (state->answered) co_return;
            state->answered = true;
            state->deadline.cancel();
            state->done(std::move(result));
        },
        asio::detached);
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
