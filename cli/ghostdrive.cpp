// ghostdrive: sets up a drive and talks to the running daemon.

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "autostart.hpp"
#include "ghostdrive/daemon/control.hpp"
#include "ghostdrive/daemon/drive.hpp"
#include "ghostdrive/daemon/invite.hpp"
#include "ghostdrive/daemon/service.hpp"

extern char** environ;

namespace {

using namespace ghostdrive;

constexpr const char* kUsage =
    "usage: ghostdrive <command> [options]\n"
    "\n"
    "Setting up:\n"
    "  init [--name NAME]               create a new drive on this device\n"
    "  invite [--ttl SECONDS]           print a one-time code for adding another device\n"
    "  join CODE [--name NAME] [--port N]  add this device to a drive using an invite code\n"
    "  rendezvous [HOST:PORT FINGERPRINT]  show or set the rendezvous server, for reaching\n"
    "                                   devices on other networks (see ghostdrive-rendezvous)\n"
    "\n"
    "Running (start the daemon with `ghostdrived`):\n"
    "  status [--json]                  show this device and the other devices\n"
    "  remove NAME|ID                   take a device out of the drive for good (a lost or\n"
    "                                   stolen one); the other devices learn of it\n"
    "  stop                             unmount and stop the daemon\n"
    "  id                               print this device's ID\n"
    "\n"
    "Starting at login:\n"
    "  autostart on [--mount DIR]       start ghostdrived at login (launchd or systemd --user)\n"
    "  autostart off                    stop starting it at login, and stop it now\n"
    "  autostart status                 show whether it is set up and running\n"
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
    std::filesystem::path mount = daemon::default_mountpoint();
    std::string name = host_name();
    std::uint16_t port = daemon::kDefaultPort;
    long ttl = 600;
    bool json = false;
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
        } else if (a == "--mount") {
            auto v = value();
            if (!v) return false;
            args.mount = v;
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
        } else if (a == "--json") {
            args.json = true;
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

int cmd_rendezvous(const Args& args) {
    auto drive = daemon::Drive::open(args.data);
    if (!drive) return fail(drive.error().message);
    if (args.positional.empty()) {
        auto config = drive.value()->rendezvous();
        std::cout << (config ? config->to_string() : std::string("none")) << "\n";
        return 0;
    }
    if (args.positional.size() < 2)
        return fail("rendezvous needs HOST:PORT and the server's fingerprint");
    std::string fingerprint;
    for (std::size_t i = 1; i < args.positional.size(); ++i) fingerprint += args.positional[i];
    auto config = daemon::RendezvousConfig::parse(args.positional[0] + " " + fingerprint);
    if (!config) return fail("expected HOST:PORT followed by the fingerprint the server printed");
    if (auto st = drive.value()->set_rendezvous(*config); !st) return fail(st.error().message);
    std::cout
        << "Rendezvous server set to " << config->to_string()
        << ".\nA running daemon picks it up within a few seconds. Devices that join from now on\n"
           "get it automatically; run this on devices that joined before.\n";
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
        return fail(
            "could not join: " + (result ? result->error().message : std::string("no answer")) +
            "\nThe invite may have expired or been used already; ask for a new one with "
            "`ghostdrive invite`, and check that both devices are on the same network.");
    }

    auto drive =
        daemon::Drive::join(args.data, args.name, result->value().info, invite.value().inviter);
    core::secure_zero(result->value().info.secret);
    for (auto& k : result->value().info.discovery_keys) core::secure_zero(k.key);
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

// Runs a program found on PATH without a shell and returns its exit status, or -1.
int run(const std::vector<std::string>& command, bool quiet) {
    std::vector<char*> argv;
    for (const auto& a : command) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (quiet) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return -1;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string join_command(const std::vector<std::string>& command) {
    std::string out;
    for (const auto& a : command) out += (out.empty() ? "" : " ") + a;
    return out;
}

std::optional<std::filesystem::path> self_path() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
    buf.resize(std::strlen(buf.c_str()));
    std::filesystem::path p = buf;
#else
    std::filesystem::path p = "/proc/self/exe";
#endif
    std::error_code ec;
    auto resolved = std::filesystem::canonical(p, ec);
    if (ec) return std::nullopt;
    return resolved;
}

std::filesystem::path absolute_path(const std::filesystem::path& p) {
    std::error_code ec;
    auto out = std::filesystem::weakly_canonical(std::filesystem::absolute(p, ec), ec);
    return ec ? std::filesystem::absolute(p).lexically_normal() : out;
}

int cmd_autostart(const Args& args) {
    if (args.positional.size() != 1) return fail("autostart needs on, off or status");
    const std::string& action = args.positional[0];
    if (action != "on" && action != "off" && action != "status") {
        return fail("autostart needs on, off or status");
    }
    if (::geteuid() == 0) {
        return fail("autostart sets up a per-user login item; run it as yourself, not as root");
    }
    const char* home_env = std::getenv("HOME");
    if (!home_env || !*home_env) return fail("HOME is not set");
    const std::filesystem::path home = home_env;
    const auto data = absolute_path(args.data);

#ifdef __APPLE__
    const std::string label = autostart::launchd_label(data);
    const auto file = autostart::launch_agent_path(home, label);
    const std::string domain = "gui/" + std::to_string(::getuid());
    const std::string service = domain + "/" + label;
    auto loaded = [&] { return run({"launchctl", "print", service}, true) == 0; };
#else
    const std::string unit = autostart::systemd_unit_name(data);
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const auto file = autostart::systemd_unit_path(
        xdg && *xdg ? std::filesystem::path(xdg) : home / ".config", unit);
    auto loaded = [&] {
        return run({"systemctl", "--user", "is-active", "--quiet", unit}, true) == 0;
    };
#endif

    std::error_code ec;
    if (action == "status") {
        bool exists = std::filesystem::exists(file, ec);
        bool running = loaded();
        std::cout << "login item: " << (exists ? file.string() : std::string("not set up")) << "\n"
#ifdef __APPLE__
                  << "launchd:    " << (running ? "loaded" : "not loaded") << "\n";
#else
                  << "systemd:    " << (running ? "active" : "inactive") << "\n";
#endif
        return 0;
    }

    if (action == "off") {
#ifdef __APPLE__
        if (loaded()) {
            std::vector<std::string> bootout{"launchctl", "bootout", service};
            if (run(bootout, false) != 0) return fail("`" + join_command(bootout) + "` failed");
        }
#else
        if (std::filesystem::exists(file, ec)) {
            std::vector<std::string> disable{"systemctl", "--user", "disable", "--now", unit};
            if (run(disable, false) != 0) return fail("`" + join_command(disable) + "` failed");
        }
#endif
        // Stopping returns at once, but the daemon still has to unmount; wait so the mount is
        // gone when this returns.
        bool stopped = true;
        for (int i = 0; i < 300; ++i) {
            stopped = !daemon::control_request(daemon::DrivePaths(data).control, "ping");
            if (stopped) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!stopped) std::cerr << "ghostdrive: the daemon is still shutting down\n";
        bool removed = std::filesystem::remove(file, ec);
        if (ec) return fail("remove " + file.string() + ": " + ec.message());
#ifndef __APPLE__
        if (removed) run({"systemctl", "--user", "daemon-reload"}, false);
#endif
        std::cout << (removed ? "GhostDrive will no longer start at login.\n"
                              : "GhostDrive was not set to start at login.\n");
        return 0;
    }

    if (!daemon::Drive::exists(data)) {
        return fail("no drive at " + data.string() +
                    "; run `ghostdrive init` or `ghostdrive join` first");
    }
    auto self = self_path();
    if (!self) return fail("cannot find the path of the running ghostdrive binary");
    autostart::Spec spec;
    spec.daemon = self->parent_path() / "ghostdrived";
    // In a build tree the two binaries sit in sibling directories (cli/ and daemon/).
    if (auto built = self->parent_path().parent_path() / "daemon" / "ghostdrived";
        ::access(spec.daemon.c_str(), X_OK) != 0 && ::access(built.c_str(), X_OK) == 0) {
        spec.daemon = built;
    }
    spec.data = data;
    spec.mount = absolute_path(args.mount);
    spec.log = data / "ghostdrived.log";
    if (::access(spec.daemon.c_str(), X_OK) != 0) {
        return fail("cannot find ghostdrived next to ghostdrive (" + spec.daemon.string() + ")");
    }
#ifdef __APPLE__
    auto text = autostart::launch_agent_plist(label, spec);
#else
    auto text = autostart::systemd_unit(spec);
#endif
    if (!text) return fail(text.error().message);
    if (auto st = autostart::write_file_atomic(file, text.value()); !st) {
        return fail(st.error().message);
    }
#ifdef __APPLE__
    // Reload so a changed plist takes effect; enable clears an earlier `launchctl disable`.
    if (loaded()) run({"launchctl", "bootout", service}, false);
    run({"launchctl", "enable", service}, true);
    std::vector<std::string> start{"launchctl", "bootstrap", domain, file.string()};
#else
    run({"systemctl", "--user", "daemon-reload"}, false);
    std::vector<std::string> start{"systemctl", "--user", "enable", "--now", unit};
#endif
    if (run(start, false) != 0) return fail("`" + join_command(start) + "` failed");
    std::cout << "GhostDrive will start at login and is starting now.\n"
              << "  login item: " << file.string() << "\n"
              << "  mount:      " << spec.mount.string() << "\n"
#ifdef __APPLE__
              << "  log:        " << spec.log.string() << "\n"
#endif
              << "Turn it off with `ghostdrive autostart off`.\n";
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
    if (args.command == "rendezvous") return cmd_rendezvous(args);
    if (args.command == "autostart") return cmd_autostart(args);
    if (args.command == "status") return cmd_control(args, args.json ? "status-json" : "status");
    if (args.command == "stop") return cmd_control(args, "stop");
    if (args.command == "remove") {
        if (args.positional.size() != 1) {
            std::cerr << "ghostdrive: remove needs the device's name or ID\n";
            return 2;
        }
        return cmd_control(args, "remove " + args.positional[0]);
    }
    std::cerr << kUsage;
    return 2;
}
