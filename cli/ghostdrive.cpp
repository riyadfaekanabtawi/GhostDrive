// ghostdrive: sets up a drive and talks to the running daemon.

#include <unistd.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "ghostdrive/daemon/control.hpp"
#include "ghostdrive/daemon/drive.hpp"
#include "ghostdrive/daemon/invite.hpp"
#include "ghostdrive/daemon/service.hpp"

namespace {

using namespace ghostdrive;

constexpr const char* kUsage =
    "usage: ghostdrive <command> [options]\n"
    "\n"
    "Setting up:\n"
    "  init [--name NAME]               create a new drive on this device\n"
    "  invite [--ttl SECONDS]           print a one-time code for adding another device\n"
    "  join CODE [--name NAME] [--port N]  add this device to a drive using an invite code\n"
    "\n"
    "Running (start the daemon with `ghostdrived`):\n"
    "  status                           show this device and the other devices\n"
    "  stop                             unmount and stop the daemon\n"
    "  id                               print this device's ID\n"
    "\n"
    "Every command accepts --data DIR to use a data directory other than the default.\n";

std::string host_name() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "device";
    std::string name(buf);
    if (auto dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    return name.empty() ? "device" : name;
}

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::filesystem::path data = daemon::default_data_dir();
    std::string name = host_name();
    std::uint16_t port = daemon::kDefaultPort;
    long ttl = 600;
};

bool parse_number(const char* text, auto& out) {
    std::string_view s(text);
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), out);
    return err == std::errc{} && end == s.data() + s.size();
}

bool parse_args(int argc, char** argv, Args& args) {
    if (argc < 2) return false;
    args.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--data") {
            auto v = value();
            if (!v) return false;
            args.data = v;
        } else if (a == "--name") {
            auto v = value();
            if (!v) return false;
            args.name = v;
        } else if (a == "--port") {
            auto v = value();
            if (!v || !parse_number(v, args.port)) return false;
        } else if (a == "--ttl") {
            auto v = value();
            if (!v || !parse_number(v, args.ttl)) return false;
        } else if (!a.empty() && a[0] == '-') {
            return false;
        } else {
            args.positional.push_back(a);
        }
    }
    return true;
}

int fail(const std::string& message) {
    std::cerr << "ghostdrive: " << message << "\n";
    return 1;
}

int cmd_init(const Args& args) {
    auto drive = daemon::Drive::create(args.data, args.name);
    if (!drive) return fail(drive.error().message);
    std::cout << "Created a new drive on " << args.name << ".\n"
              << "  device ID: " << core::fingerprint(drive.value()->key().public_key()) << "\n"
              << "  data:      " << args.data.string() << "\n\n"
              << "Start it with `ghostdrived`, then add devices with `ghostdrive invite`.\n";
    return 0;
}

int cmd_id(const Args& args) {
    auto key = core::DeviceKey::load(daemon::DrivePaths(args.data).key);
    if (!key) return fail("no device key yet; run `ghostdrive init` or `ghostdrive join`");
    std::cout << core::fingerprint(key.value().public_key()) << "\n";
    return 0;
}

int cmd_control(const Args& args, const std::string& command) {
    auto reply = daemon::control_request(daemon::DrivePaths(args.data).control, command);
    if (!reply) return fail(reply.error().message + "\nStart it with `ghostdrived`.");
    std::cout << reply.value();
    return reply.value().rfind("error:", 0) == 0 ? 1 : 0;
}

int cmd_invite(const Args& args) {
    auto reply = daemon::control_request(daemon::DrivePaths(args.data).control,
                                         "invite " + std::to_string(args.ttl));
    if (!reply) return fail(reply.error().message + "\nStart it with `ghostdrived`.");
    if (reply.value().rfind("error:", 0) == 0) return fail(reply.value());
    std::cout << "On the other device, run:\n\n  ghostdrive join " << reply.value() << "\n"
              << "The code works once, for " << args.ttl / 60
              << " minutes. Anyone who has it can join, so send it privately.\n";
    return 0;
}

int cmd_join(const Args& args) {
    if (args.positional.size() != 1) return fail("join needs the invite code");
    if (daemon::Drive::exists(args.data)) {
        return fail("this device already belongs to a drive (" + args.data.string() + ")");
    }
    auto invite = daemon::decode_invite(args.positional[0]);
    if (!invite) return fail(invite.error().message);
    auto key = daemon::Drive::load_or_create_key(args.data);
    if (!key) return fail(key.error().message);

    asio::io_context io;
    transport::EndpointOptions options;
    options.bind = asio::ip::udp::endpoint(asio::ip::address_v4::any(), 0);
    options.handshake_timeout = std::chrono::seconds(5);
    auto endpoint = transport::Endpoint::create(io.get_executor(), key.value(), options);
    if (!endpoint) return fail(endpoint.error().message);

    std::optional<core::Result<daemon::JoinResult>> result;
    daemon::Join join;
    join.token = invite.value().token;
    join.name = args.name;
    join.listen_port = args.port;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            result = co_await daemon::request_join(*endpoint.value(), invite.value(), join);
            endpoint.value()->close();
        },
        asio::detached);
    std::cout << "Contacting the inviting device...\n" << std::flush;
    io.run();
    if (!result || !*result) {
        return fail("could not join: " +
                    (result ? result->error().message : std::string("no answer")) +
                    "\nThe invite may have expired or been used already; ask for a new one with "
                    "`ghostdrive invite`, and check that both devices are on the same network.");
    }

    auto drive = daemon::Drive::join(args.data, args.name, result->value().info);
    core::secure_zero(result->value().info.secret);
    if (!drive) return fail(drive.error().message);
    auto address = result->value().inviter_address;
    auto text = address.address().to_string() + ":" + std::to_string(address.port());
    if (auto st = drive.value()->set_peer_address(invite.value().inviter, text); !st) {
        return fail(st.error().message);
    }
    std::cout << "Joined the drive as " << args.name << ".\n"
              << "Start it with `ghostdrived`; files appear in ~/GhostDrive.\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::cerr << kUsage;
        return 2;
    }
    if (args.command == "init") return cmd_init(args);
    if (args.command == "id") return cmd_id(args);
    if (args.command == "invite") return cmd_invite(args);
    if (args.command == "join") return cmd_join(args);
    if (args.command == "status") return cmd_control(args, "status");
    if (args.command == "stop") return cmd_control(args, "stop");
    std::cerr << kUsage;
    return 2;
}
