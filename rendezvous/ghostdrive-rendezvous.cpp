// ghostdrive-rendezvous: the self-hostable rendezvous and relay server.

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/identity.hpp"
#include "ghostdrive/rendezvous/server.hpp"

namespace {

using namespace ghostdrive;

constexpr const char* kUsage =
    "usage: ghostdrive-rendezvous [--key FILE] [--port N] [--allow-group HEX]...\n"
    "\n"
    "Runs a rendezvous and relay server on UDP port N (default 7374). The server's key is kept\n"
    "in FILE (default ./rendezvous.key, created on first run). Devices are pointed at it with\n"
    "  ghostdrive rendezvous HOST:PORT FINGERPRINT\n"
    "using the fingerprint printed at startup. With --allow-group, only those drives may use the\n"
    "server; `ghostdrive status` on a device shows its drive's group.\n";

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path key_path = "rendezvous.key";
    std::uint16_t port = rendezvous::kDefaultPort;
    rendezvous::ServerOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--key" && i + 1 < argc) {
            key_path = argv[++i];
        } else if (a == "--allow-group" && i + 1 < argc) {
            auto bytes = core::from_hex(argv[++i]);
            if (!bytes || bytes->size() != discovery::kGroupIdSize) {
                std::cerr << "ghostdrive-rendezvous: --allow-group takes a 64-digit hex group\n";
                return 2;
            }
            discovery::GroupId group{};
            std::copy(bytes->begin(), bytes->end(), group.begin());
            options.allowed_groups.insert(group);
        } else if (a == "--port" && i + 1 < argc) {
            std::string_view s(argv[++i]);
            auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), port);
            if (err != std::errc{} || end != s.data() + s.size()) {
                std::cerr << kUsage;
                return 2;
            }
        } else {
            std::cerr << kUsage;
            return 2;
        }
    }

    auto key = core::DeviceKey::load(key_path);
    if (!key && key.error().code == core::Errc::not_found) {
        key = core::DeviceKey::generate();
        if (key) {
            if (auto st = key.value().save(key_path); !st) {
                std::cerr << "ghostdrive-rendezvous: " << st.error().message << "\n";
                return 1;
            }
        }
    }
    if (!key) {
        std::cerr << "ghostdrive-rendezvous: " << key.error().message << "\n";
        return 1;
    }

    asio::io_context io;
    options.listen = asio::ip::udp::endpoint(asio::ip::address_v4::any(), port);
    auto server = rendezvous::Server::start(io.get_executor(), key.value(), options);
    if (!server) {
        std::cerr << "ghostdrive-rendezvous: " << server.error().message << "\n";
        return 1;
    }
    std::cout << "GhostDrive rendezvous server listening on UDP port " << server.value()->port()
              << "\nFingerprint: " << core::fingerprint(key.value().public_key())
              << "\nOn each device: ghostdrive rendezvous <this-host>:" << server.value()->port()
              << " " << core::fingerprint(key.value().public_key()) << "\n"
              << std::flush;

    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code&, int) {
        server.value()->stop();
        io.stop();
    });
    io.run();
    return 0;
}
