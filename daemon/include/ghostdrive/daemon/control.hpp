#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/local/stream_protocol.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "ghostdrive/core/result.hpp"

namespace ghostdrive::daemon {

// The daemon's local control API: a Unix socket in the 0700 data directory, so only the owning
// user reaches it. One request per connection: a command line in, a text reply out.
class ControlServer {
public:
    // Runs on the server's executor; returns the reply text.
    using Handler = std::function<std::string(const std::string& command)>;

    static core::Result<std::unique_ptr<ControlServer>> start(asio::any_io_executor executor,
                                                              const std::filesystem::path& socket,
                                                              Handler handler);
    ~ControlServer();
    void stop();

    struct Private {};
    ControlServer(Private, asio::any_io_executor executor, std::filesystem::path socket,
                  Handler handler);

private:
    void accept_next();

    std::filesystem::path path_;
    Handler handler_;
    asio::local::stream_protocol::acceptor acceptor_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// Sends one command to a running daemon and returns its reply.
core::Result<std::string> control_request(const std::filesystem::path& socket,
                                          const std::string& command);

}  // namespace ghostdrive::daemon
