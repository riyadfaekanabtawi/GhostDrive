#include "autostart.hpp"

#include <blake3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace ghostdrive::autostart {

using core::Errc;
using core::Error;

namespace {

bool is_control(char c) {
    auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f;
}

Error control_error(std::string_view text) {
    return Error{Errc::invalid_argument, "path contains a control character: " + std::string(text)};
}

Error errno_error(const std::string& what, const std::filesystem::path& path) {
    return Error{Errc::io, what + " " + path.string() + ": " + std::strerror(errno)};
}

}  // namespace

std::string data_dir_id(const std::filesystem::path& data) {
    const std::string text = data.string();
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, text.data(), text.size());
    std::array<unsigned char, 8> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 0xf];
    }
    return out;
}

std::string launchd_label(const std::filesystem::path& data) {
    return "io.ghostdrive.daemon." + data_dir_id(data);
}

std::string systemd_unit_name(const std::filesystem::path& data) {
    return "ghostdrive-" + data_dir_id(data) + ".service";
}

std::filesystem::path launch_agent_path(const std::filesystem::path& home,
                                        const std::string& label) {
    return home / "Library" / "LaunchAgents" / (label + ".plist");
}

std::filesystem::path systemd_unit_path(const std::filesystem::path& config_home,
                                        const std::string& unit) {
    return config_home / "systemd" / "user" / unit;
}

core::Result<std::string> xml_escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (is_control(c)) return control_error(text);
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

core::Result<std::string> systemd_quote(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        if (is_control(c)) return control_error(text);
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '%':
                out += "%%";
                break;
            case '$':
                out += "$$";
                break;
            default:
                out += c;
        }
    }
    out += '"';
    return out;
}

core::Result<std::string> launch_agent_plist(const std::string& label, const Spec& spec) {
    auto l = xml_escape(label);
    auto daemon = xml_escape(spec.daemon.string());
    auto data = xml_escape(spec.data.string());
    auto mount = xml_escape(spec.mount.string());
    auto log = xml_escape(spec.log.string());
    for (auto* r : {&l, &daemon, &data, &mount, &log}) {
        if (!*r) return r->error();
    }
    // KeepAlive restarts only after an unsuccessful exit, so `ghostdrive stop` (exit 0) stays
    // stopped until the next login.
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n"
           "<dict>\n"
           "\t<key>Label</key>\n"
           "\t<string>" +
           l.value() +
           "</string>\n"
           "\t<key>ProgramArguments</key>\n"
           "\t<array>\n"
           "\t\t<string>" +
           daemon.value() +
           "</string>\n"
           "\t\t<string>--data</string>\n"
           "\t\t<string>" +
           data.value() +
           "</string>\n"
           "\t\t<string>--mount</string>\n"
           "\t\t<string>" +
           mount.value() +
           "</string>\n"
           "\t</array>\n"
           "\t<key>RunAtLoad</key>\n"
           "\t<true/>\n"
           "\t<key>KeepAlive</key>\n"
           "\t<dict>\n"
           "\t\t<key>SuccessfulExit</key>\n"
           "\t\t<false/>\n"
           "\t</dict>\n"
           "\t<key>ProcessType</key>\n"
           "\t<string>Standard</string>\n"
           "\t<key>StandardOutPath</key>\n"
           "\t<string>" +
           log.value() +
           "</string>\n"
           "\t<key>StandardErrorPath</key>\n"
           "\t<string>" +
           log.value() +
           "</string>\n"
           "</dict>\n"
           "</plist>\n";
}

core::Result<std::string> systemd_unit(const Spec& spec) {
    auto daemon = systemd_quote(spec.daemon.string());
    auto data = systemd_quote(spec.data.string());
    auto mount = systemd_quote(spec.mount.string());
    for (auto* r : {&daemon, &data, &mount}) {
        if (!*r) return r->error();
    }
    return "[Unit]\n"
           "Description=GhostDrive daemon\n"
           "\n"
           "[Service]\n"
           "ExecStart=" +
           daemon.value() + " --data " + data.value() + " --mount " + mount.value() +
           "\n"
           "Restart=on-failure\n"
           "RestartSec=10\n"
           "\n"
           "[Install]\n"
           "WantedBy=default.target\n";
}

core::Status write_file_atomic(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return Error{Errc::io, "create " + path.parent_path().string() + ": " + ec.message()};
    auto tmp = path;
    tmp += ".tmp";
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return errno_error("open", tmp);
    auto fail = [&](const char* what) {
        auto err = errno_error(what, tmp);
        ::close(fd);
        ::unlink(tmp.c_str());
        return err;
    };
    // The umask may have narrowed the mode given to open().
    if (::fchmod(fd, 0644) != 0) return fail("chmod");
    const char* p = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail("write");
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) return fail("fsync");
    if (::close(fd) != 0 || ::rename(tmp.c_str(), path.c_str()) != 0) {
        auto err = errno_error("rename", tmp);
        ::unlink(tmp.c_str());
        return err;
    }
    return {};
}

}  // namespace ghostdrive::autostart
