// ghostdrived: runs one device of a GhostDrive drive and mounts it.

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/signal_set.hpp>
#include <asio/thread_pool.hpp>
#include <charconv>
#include <cstdio>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "ghostdrive/daemon/control.hpp"
#include "ghostdrive/daemon/drive.hpp"
#include "ghostdrive/daemon/service.hpp"
#include "ghostdrive/version.hpp"
#include "status_json.hpp"
#include "status_text.hpp"
#ifdef GHOSTDRIVE_HAVE_FUSE
#include "ghostdrive/vfs/fuse_mount.hpp"
#endif

namespace {

using namespace ghostdrive;

constexpr const char* kUsage =
    "usage: ghostdrived [--data DIR] [--mount PATH | --no-mount] [--port N] [--no-lan]\n"
    "       ghostdrived --version\n"
    "\n"
    "Runs this device of the drive in DIR (default: the per-user data directory) and mounts\n"
    "it at PATH (default: ~/GhostDrive). Stop with Ctrl-C or `ghostdrive stop`.\n";

struct Args {
    std::filesystem::path data = daemon::default_data_dir();
    std::filesystem::path mountpoint = daemon::default_mountpoint();
    bool mount = true;
    std::uint16_t port = daemon::kDefaultPort;
    bool lan = true;
};

bool parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--data") {
            auto v = value();
            if (!v) return false;
            args.data = v;
        } else if (a == "--mount") {
            auto v = value();
            if (!v) return false;
            args.mountpoint = v;
        } else if (a == "--no-mount") {
            args.mount = false;
        } else if (a == "--no-lan") {
            args.lan = false;
        } else if (a == "--port") {
            auto v = value();
            if (!v) return false;
            std::string_view s(v);
            auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), args.port);
            if (err != std::errc{} || end != s.data() + s.size()) return false;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "ghostdrived " << kVersion << "\n";
        return 0;
    }
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::cerr << kUsage;
        return 2;
    }
#ifndef GHOSTDRIVE_HAVE_FUSE
    if (args.mount) {
        std::cerr << "ghostdrived was built without FUSE support; run with --no-mount\n";
        return 1;
    }
#endif

    auto drive = daemon::Drive::open(args.data);
    if (!drive) {
        std::cerr
            << "ghostdrived: " << drive.error().message
            << "\nCreate a drive with `ghostdrive init` or join one with `ghostdrive join`.\n";
        return 1;
    }

    asio::io_context io;
    auto work = asio::make_work_guard(io);
    asio::thread_pool disk(4);

    daemon::ServiceOptions options;
    options.listen = asio::ip::udp::endpoint(asio::ip::address_v4::any(), args.port);
    options.lan_discovery = args.lan;
    daemon::Service service(io.get_executor(), disk.get_executor(), *drive.value(), options);
    if (auto st = service.start(); !st) {
        std::cerr << "ghostdrived: " << st.error().message << "\n";
        return 1;
    }

    std::promise<void> stop_requested;
    std::once_flag stop_once;
    auto request_stop = [&] { std::call_once(stop_once, [&] { stop_requested.set_value(); }); };

    std::string mounted_at = args.mount ? args.mountpoint.string() : "";
    auto control = daemon::ControlServer::start(
        io.get_executor(), drive.value()->paths().control,
        [&](const std::string& command) -> std::string {
            if (command == "ping") return "pong\n";
            if (command == "status") return daemon::format_status(service.status(), mounted_at);
            if (command == "status-json") {
                return daemon::format_status_json(service.status(), mounted_at);
            }
            if (command == "stop") {
                request_stop();
                return "stopping\n";
            }
            if (command.rfind("remove ", 0) == 0) {
                auto removed = service.remove_member(command.substr(7));
                return removed ? removed.value() : "error: " + removed.error().message + "\n";
            }
            if (command.rfind("invite", 0) == 0) {
                long ttl = 600;
                if (command.size() > 7) {
                    auto s = std::string_view(command).substr(7);
                    std::from_chars(s.data(), s.data() + s.size(), ttl);
                }
                auto code =
                    service.create_invite(std::chrono::seconds(std::clamp(ttl, 30L, 86400L)));
                return code ? code.value() + "\n" : "error: " + code.error().message + "\n";
            }
            return "error: unknown command\n";
        });
    if (!control) {
        std::cerr << "ghostdrived: " << control.error().message << "\n";
        return 1;
    }

    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code& ec, int) {
        if (!ec) request_stop();
    });

    std::thread io_thread([&] { io.run(); });

#ifdef GHOSTDRIVE_HAVE_FUSE
    std::unique_ptr<vfs::FuseMount> mount;
    if (args.mount) {
        std::error_code ec;
        std::filesystem::create_directories(args.mountpoint, ec);
        vfs::FuseMountOptions mount_options;
        mount_options.volume_name = "GhostDrive";
        auto mounted = vfs::FuseMount::mount(service.filesystem(), args.mountpoint, mount_options);
        if (!mounted) {
            std::cerr << "ghostdrived: " << mounted.error().message << "\n";
            request_stop();
        } else {
            mount = std::move(mounted).value();
        }
    }
#endif

    std::cout << "GhostDrive is running.\n"
              << "  device:  " << drive.value()->device_name() << " ("
              << core::fingerprint(drive.value()->key().public_key()) << ")\n"
              << "  mounted: " << (mounted_at.empty() ? "no" : mounted_at) << "\n"
              << "  port:    UDP " << service.port() << "\n"
              << std::flush;

    stop_requested.get_future().wait();
    std::cout << "Stopping...\n" << std::flush;

#ifdef GHOSTDRIVE_HAVE_FUSE
    // Unmount while the executor still runs, so in-flight reads and commits can finish.
    mount.reset();
#endif
    std::promise<void> stopped;
    asio::post(io, [&] {
        signals.cancel();
        control.value()->stop();
        service.stop();
        stopped.set_value();
    });
    stopped.get_future().wait();
    // Let close packets and final commits drain, then end the loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    work.reset();
    io.stop();
    io_thread.join();
    disk.join();
    std::cout << "Stopped.\n" << std::flush;
    return 0;
}
