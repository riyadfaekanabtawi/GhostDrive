#include "ghostdrive/daemon/control.hpp"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#ifdef _WIN32
// After Asio, which includes winsock2.h, a prerequisite of afunix.h.
#include <afunix.h>

#include "ghostdrive/core/file.hpp"
#endif

namespace ghostdrive::daemon {

using core::Errc;
using core::Error;
using Local = asio::local::stream_protocol;

namespace {

constexpr std::size_t kMaxCommand = 1024;

// True if the process at the other end of `fd` runs as this user. The socket's directory
// already keeps other users out; this also covers a socket file someone else put in place.
bool same_user(Local::socket::native_handle_type fd) {
#ifdef _WIN32
    // Windows has no peer credentials for AF_UNIX, but it reports the peer's process ID.
    ULONG pid = 0;
    DWORD returned = 0;
    if (::WSAIoctl(fd, SIO_AF_UNIX_GETPEERPID, nullptr, 0, &pid, sizeof(pid), &returned, nullptr,
                   nullptr) != 0) {
        return false;
    }
    return core::process_is_current_user(pid);
#else
#ifdef __APPLE__
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0) return false;
#else
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    uid_t uid = cred.uid;
#endif
    return uid == ::geteuid();
#endif
}

// Constructing an endpoint from a longer path throws; check first.
bool fits(const std::filesystem::path& socket) {
    return socket.string().size() < sizeof(sockaddr_un::sun_path);
}

asio::awaitable<void> serve(Local::socket socket, ControlServer::Handler handler,
                            std::shared_ptr<bool> alive) {
    asio::streambuf buffer(kMaxCommand);
    std::error_code ec;
    if (!same_user(socket.native_handle())) co_return;
    co_await asio::async_read_until(socket, buffer, '\n',
                                    asio::redirect_error(asio::use_awaitable, ec));
    if (ec || !*alive) co_return;
    std::string command(asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
    while (!command.empty() && (command.back() == '\n' || command.back() == '\r'))
        command.pop_back();
    auto reply = handler(command);
    co_await asio::async_write(socket, asio::buffer(reply),
                               asio::redirect_error(asio::use_awaitable, ec));
    socket.shutdown(Local::socket::shutdown_both, ec);
}

}  // namespace

ControlServer::ControlServer(Private, asio::any_io_executor executor, std::filesystem::path socket,
                             Handler handler)
    : path_(std::move(socket)), handler_(std::move(handler)), acceptor_(executor) {}

ControlServer::~ControlServer() {
    *alive_ = false;
    stop();
}

core::Result<std::unique_ptr<ControlServer>> ControlServer::start(
    asio::any_io_executor executor, const std::filesystem::path& socket, Handler handler) {
    // A socket file left by a daemon that did not exit cleanly blocks bind; a live daemon still
    // answering on it means another one is already running.
    if (!fits(socket)) return Error{Errc::invalid_argument, "control socket path is too long"};
    std::error_code ec;
    // symlink_status: on Windows the socket file is a reparse point that status() cannot open.
    if (std::filesystem::exists(std::filesystem::symlink_status(socket, ec))) {
        if (control_request(socket, "ping")) {
            return Error{Errc::already_exists, "a daemon is already running for this drive"};
        }
        std::filesystem::remove(socket, ec);
    }
    auto server = std::make_unique<ControlServer>(Private{}, executor, socket, std::move(handler));
    ec.clear();
    auto& acceptor = server->acceptor_;
    acceptor.open(Local(), ec);
    if (!ec) acceptor.bind(Local::endpoint(socket.string()), ec);
#ifndef _WIN32
    // On Windows the socket file inherits the private directory's owner-only access list.
    if (!ec) ::chmod(socket.c_str(), 0600);
#endif
    if (!ec) acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) return Error{Errc::io, "control socket " + socket.string() + ": " + ec.message()};
    server->accept_next();
    return server;
}

void ControlServer::accept_next() {
    acceptor_.async_accept([this, alive = alive_](const std::error_code& ec, Local::socket socket) {
        if (!*alive || ec == asio::error::operation_aborted) return;
        if (!ec) {
            asio::co_spawn(acceptor_.get_executor(), serve(std::move(socket), handler_, alive),
                           asio::detached);
        }
        accept_next();
    });
}

void ControlServer::stop() {
    if (!acceptor_.is_open()) return;
    std::error_code ec;
    acceptor_.close(ec);
    std::filesystem::remove(path_, ec);
}

core::Result<std::string> control_request(const std::filesystem::path& socket,
                                          const std::string& command) {
    if (!fits(socket)) return Error{Errc::invalid_argument, "control socket path is too long"};
    asio::io_context io;
    Local::socket s(io);
    std::error_code ec;
    s.connect(Local::endpoint(socket.string()), ec);
    if (ec) {
        return Error{Errc::closed, "the GhostDrive daemon is not running (" + ec.message() + ")"};
    }
    if (!same_user(s.native_handle())) {
        return Error{Errc::permission, "the control socket belongs to another user"};
    }
    std::string line = command + "\n";
    asio::write(s, asio::buffer(line), ec);
    if (ec) return Error{Errc::io, "control request failed: " + ec.message()};
    std::string reply;
    asio::read(s, asio::dynamic_buffer(reply), ec);
    if (ec && ec != asio::error::eof)
        return Error{Errc::io, "control reply failed: " + ec.message()};
    return reply;
}

}  // namespace ghostdrive::daemon
