#include "ghostdrive/daemon/control.hpp"

#include <gtest/gtest.h>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <thread>

#include "ghostdrive/core/file.hpp"
#include "ghostdrive/daemon/drive.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::daemon {
namespace {

// The control socket sits in a private data directory and is placed as the daemon's is.
class Control : public ::testing::Test {
protected:
    Control() : work_(asio::make_work_guard(io_)) {
        EXPECT_TRUE(core::make_private_dir(dir_.path()));
        socket_ = DrivePaths(dir_.path()).control;
    }
    void SetUp() override {
        // Where neither the data directory nor a private temporary directory gives a path that
        // fits in sun_path, the daemon cannot listen either.
        if (socket_.string().size() >= 104) GTEST_SKIP() << "socket path too long: " << socket_;
    }
    ~Control() override {
        work_.reset();
        io_.stop();
        if (thread_.joinable()) thread_.join();
    }

    core::Result<std::unique_ptr<ControlServer>> start() {
        return ControlServer::start(io_.get_executor(), socket_, [](const std::string& command) {
            return "got " + command + "\n";
        });
    }
    // Starts a server that lives until the fixture ends, and runs its executor.
    void serve() {
        auto server = start();
        ASSERT_TRUE(server) << server.error().message;
        server_ = std::move(server).value();
        thread_ = std::thread([this] { io_.run(); });
    }

    test::TempDir dir_;
    std::filesystem::path socket_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::unique_ptr<ControlServer> server_;  // destroyed after the executor thread has ended
    std::thread thread_;
};

TEST_F(Control, Answers) {
    ASSERT_NO_FATAL_FAILURE(serve());
    auto reply = control_request(socket_, "ping");
    ASSERT_TRUE(reply) << reply.error().message;
    EXPECT_EQ(reply.value(), "got ping\n");
    auto again = control_request(socket_, "status");
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(again.value(), "got status\n");
}

TEST_F(Control, RefusesASecondServer) {
    ASSERT_NO_FATAL_FAILURE(serve());
    auto second = start();
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, core::Errc::already_exists);
    EXPECT_TRUE(control_request(socket_, "ping"));
}

TEST_F(Control, NotRunning) {
    auto reply = control_request(socket_, "ping");
    ASSERT_FALSE(reply);
    EXPECT_EQ(reply.error().code, core::Errc::closed);
}

}  // namespace
}  // namespace ghostdrive::daemon
