#include "ghostdrive/app/cli.hpp"

#include <algorithm>
#include <charconv>

#include "ghostdrive/app/text.hpp"

namespace ghostdrive::app {

namespace {

constexpr std::string_view kBase32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr std::size_t kMaxNameBytes = 64;  // kMaxMemberNameBytes, daemon/include/.../codec.hpp

Argv with_data(Argv args, const Paths& p) {
    if (p.data) {
        args.push_back("--data");
        args.push_back(*p.data);
    }
    return args;
}

bool is_base32(char c) {
    return kBase32.find(c) != std::string_view::npos;
}

char upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool contains(std::string_view s, std::string_view part) {
    return s.find(part) != std::string_view::npos;
}

void replace_all(std::string& s, std::string_view from, std::string_view to) {
    for (auto pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
        s.replace(pos, from.size(), to);
    }
}

std::string parent_dir(std::string_view path) {
    auto slash = path.rfind('/');
    if (slash == std::string_view::npos) return {};
    if (slash == 0) return "/";
    return std::string(path.substr(0, slash));
}

std::string child(std::string_view dir, std::string_view name) {
    std::string out(dir);
    if (out.empty() || out.back() != '/') out += '/';
    out += name;
    return out;
}

void add_path_entries(std::vector<std::string>& out, std::string_view path_env,
                      std::string_view name) {
    std::size_t start = 0;
    while (start <= path_env.size()) {
        auto end = path_env.find(':', start);
        if (end == std::string_view::npos) end = path_env.size();
        auto dir = path_env.substr(start, end - start);
        // Empty and relative entries mean the working directory, which is not a place to run
        // programs from.
        if (!dir.empty() && dir[0] == '/') out.push_back(child(dir, name));
        start = end + 1;
    }
}

std::vector<std::string> unique(std::vector<std::string> v) {
    std::vector<std::string> out;
    for (auto& s : v) {
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

namespace commands {

Argv status(const Paths& p) {
    return with_data({"status", "--json"}, p);
}

Argv id(const Paths& p) {
    return with_data({"id"}, p);
}

Argv init(const Paths& p, const std::string& name) {
    return with_data({"init", "--name", name}, p);
}

Argv join(const Paths& p, const std::string& code, const std::string& name) {
    return with_data({"join", code, "--name", name}, p);
}

Argv invite(const Paths& p, long ttl_seconds) {
    return with_data({"invite", "--ttl", std::to_string(ttl_seconds)}, p);
}

Argv remove(const Paths& p, const std::string& id) {
    return with_data({"remove", id}, p);
}

Argv stop(const Paths& p) {
    return with_data({"stop"}, p);
}

Argv autostart_on(const Paths& p) {
    Argv args = with_data({"autostart", "on"}, p);
    if (p.mount) {
        args.push_back("--mount");
        args.push_back(*p.mount);
    }
    return args;
}

Argv autostart_off(const Paths& p) {
    return with_data({"autostart", "off"}, p);
}

Argv autostart_status(const Paths& p) {
    return with_data({"autostart", "status"}, p);
}

Argv rendezvous_show(const Paths& p) {
    return with_data({"rendezvous"}, p);
}

Argv rendezvous_set(const Paths& p, const std::string& address, const std::string& fingerprint) {
    return with_data({"rendezvous", address, fingerprint}, p);
}

Argv daemon(const Paths& p) {
    Argv args = with_data({}, p);
    if (p.mount) {
        args.push_back("--mount");
        args.push_back(*p.mount);
    }
    return args;
}

}  // namespace commands

std::optional<std::string> device_name_problem(std::string_view name) {
    std::string trimmed = trim(name);
    if (trimmed.empty()) return "Enter a name for this device.";
    if (trimmed.size() > kMaxNameBytes) return "The name is too long; use at most 64 bytes.";
    if (clean_text(trimmed, trimmed.size()) != trimmed) {
        return "The name contains control or invisible characters.";
    }
    return std::nullopt;
}

std::optional<std::string> path_problem(std::string_view value) {
    std::string trimmed = trim(value);
    if (trimmed.empty()) return std::nullopt;
    if (clean_text(trimmed, trimmed.size()) != trimmed) {
        return "The path contains control or invisible characters.";
    }
    if (trimmed[0] == '/' || trimmed == "~" || trimmed.rfind("~/", 0) == 0) return std::nullopt;
    return "Use a full path starting with / or ~.";
}

std::optional<std::string> rendezvous_address_problem(std::string_view address) {
    std::string a = trim(address);
    auto colon = a.rfind(':');
    if (a.empty() || colon == std::string::npos || colon == 0 || colon + 1 == a.size()) {
        return "Enter the server as HOST:PORT, for example rv.example.org:7374.";
    }
    auto host = std::string_view(a).substr(0, colon);
    auto digits = std::string_view(a).substr(colon + 1);
    unsigned port = 0;
    auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (err != std::errc{} || end != digits.data() + digits.size() || port == 0 || port > 65535) {
        return "The port must be a number from 1 to 65535.";
    }
    bool bad =
        host.size() > 253 || std::any_of(host.begin(), host.end(), [](char c) {
            return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '.' || c == '-' || c == ':' || c == '[' || c == ']');
        });
    if (bad) return "The host may only contain letters, digits, dots, dashes and colons.";
    return std::nullopt;
}

bool is_invite_code(std::string_view word) {
    if (word.substr(0, 4) != "gd1-") return false;
    auto rest = word.substr(4);
    if (rest.empty()) return false;
    std::size_t group = 0;
    for (char c : rest) {
        if (c == '-') {
            if (group == 0) return false;
            group = 0;
        } else if (is_base32(c)) {
            ++group;
        } else {
            return false;
        }
    }
    return group > 0;
}

std::optional<std::string> extract_invite_code(std::string_view text) {
    std::string up;
    up.reserve(text.size());
    for (char c : text) up += upper(c);
    auto start = up.find("GD1-");
    if (start == std::string::npos) return std::nullopt;
    std::string code = "gd1-";
    for (auto i = start + 4; i < up.size() && (is_base32(up[i]) || up[i] == '-'); ++i) {
        code += up[i];
    }
    while (!code.empty() && code.back() == '-') code.pop_back();
    if (!is_invite_code(code)) return std::nullopt;
    return code;
}

std::optional<std::string> normalize_fingerprint(std::string_view text) {
    std::string compact;
    for (char c : text) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        c = upper(c);
        if (!is_base32(c)) return std::nullopt;
        compact += c;
    }
    if (compact.size() != 52) return std::nullopt;
    std::string out;
    for (std::size_t i = 0; i < compact.size(); ++i) {
        if (i != 0 && i % 4 == 0) out += '-';
        out += compact[i];
    }
    return out;
}

std::optional<std::string> expand_path(std::string_view value, std::string_view home) {
    std::string trimmed = trim(value);
    if (trimmed.empty()) return std::nullopt;
    if (trimmed == "~") return std::string(home);
    if (trimmed.rfind("~/", 0) == 0) return child(home, trimmed.substr(2));
    return trimmed;
}

std::string default_data_dir(std::string_view home, std::string_view xdg_data_home) {
    if (!xdg_data_home.empty()) return child(xdg_data_home, "ghostdrive");
    return child(child(home, ".local/share"), "ghostdrive");
}

std::string default_mountpoint(std::string_view home) {
    return child(home, "GhostDrive");
}

std::string drive_marker(std::string_view data_dir) {
    return child(data_dir, "drive");
}

bool mountinfo_has(std::string_view mountinfo, std::string_view path) {
    std::string want(path);
    while (want.size() > 1 && want.back() == '/') want.pop_back();
    std::size_t start = 0;
    while (start < mountinfo.size()) {
        auto end = mountinfo.find('\n', start);
        if (end == std::string_view::npos) end = mountinfo.size();
        auto line = mountinfo.substr(start, end - start);
        start = end + 1;
        // Fields: mount ID, parent ID, major:minor, root, mount point, ...
        std::size_t field = 0, pos = 0;
        while (field < 4 && pos != std::string_view::npos) {
            pos = line.find(' ', pos);
            if (pos != std::string_view::npos) ++pos;
            ++field;
        }
        if (pos == std::string_view::npos) continue;
        auto stop = line.find(' ', pos);
        auto escaped =
            line.substr(pos, stop == std::string_view::npos ? line.size() - pos : stop - pos);
        // The kernel writes space, tab, newline and backslash as three octal digits.
        std::string point;
        for (std::size_t i = 0; i < escaped.size(); ++i) {
            if (escaped[i] == '\\' && i + 3 < escaped.size()) {
                auto oct = escaped.substr(i + 1, 3);
                if (std::all_of(oct.begin(), oct.end(),
                                [](char d) { return d >= '0' && d <= '7'; })) {
                    point += static_cast<char>((oct[0] - '0') * 64 + (oct[1] - '0') * 8 +
                                               (oct[2] - '0'));
                    i += 3;
                    continue;
                }
            }
            point += escaped[i];
        }
        if (point == want) return true;
    }
    return false;
}

std::optional<Invite> parse_invite(std::string_view text) {
    Invite invite;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find_first_of(" \t\r\n", start);
        if (end == std::string_view::npos) end = text.size();
        auto word = text.substr(start, end - start);
        if (is_invite_code(word)) {
            invite.code = std::string(word);
            break;
        }
        start = end + 1;
    }
    if (invite.code.empty()) return std::nullopt;
    // "The code works once, for 10 minutes."
    if (auto at = text.find("for "); at != std::string_view::npos) {
        auto digits = text.substr(at + 4);
        long minutes = 0;
        auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), minutes);
        auto rest = digits.substr(static_cast<std::size_t>(end - digits.data()));
        if (err == std::errc{} && rest.substr(0, 7) == " minute") invite.minutes = minutes;
    }
    return invite;
}

std::string format_countdown(long seconds) {
    if (seconds < 0) seconds = 0;
    std::string s = std::to_string(seconds % 60);
    return std::to_string(seconds / 60) + ":" + (s.size() < 2 ? "0" + s : s);
}

std::optional<AutostartState> parse_autostart_status(std::string_view text) {
    std::optional<AutostartState> out;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            auto key = trim(line.substr(0, colon));
            auto value = trim(line.substr(colon + 1));
            if (key == "login item") {
                if (!out) out.emplace();
                out->configured = value != "not set up";
            } else if (key == "systemd" || key == "launchd") {
                if (!out) out.emplace();
                out->active = value == "active" || value == "loaded";
            }
        }
        start = end + 1;
    }
    return out;
}

std::optional<RendezvousSetting> parse_rendezvous_setting(std::string_view text) {
    std::string line = trim(text);
    RendezvousSetting out;
    if (line == "none") return out;
    auto space = line.find(' ');
    if (space == std::string::npos) return std::nullopt;
    out.configured = true;
    out.address = clean_text(line.substr(0, space), 64);
    out.fingerprint = clean_text(trim(std::string_view(line).substr(space + 1)), 128);
    if (out.address.empty() || out.fingerprint.empty()) return std::nullopt;
    return out;
}

bool says_not_running(const CliResult& r) {
    return r.launched && !r.timed_out && r.exit_status != 0 &&
           (contains(r.err, "daemon is not running") ||
            contains(r.err, "Start it with `ghostdrived`"));
}

std::string plain_error(const CliResult& r) {
    if (!r.launched) {
        std::string out =
            "Could not run the ghostdrive tool. Install GhostDrive so that ghostdrive is next to "
            "this app or on PATH.";
        if (auto lines = clean_lines(r.err, 2); !lines.empty()) out += "\n" + join_lines(lines);
        return out;
    }
    if (r.timed_out) return "The ghostdrive tool did not finish in time.";
    if (r.exit_status == 2 && contains(r.err, "usage: ghostdrive")) {
        return "The ghostdrive tool did not accept the request; it may be older than this app.";
    }
    std::vector<std::string> out;
    for (auto line : clean_lines(r.err.empty() ? r.out : r.err, 8)) {
        if (contains(line, "daemon is not running")) {
            line = "GhostDrive is not running.";
        } else if (contains(line, "Start it with `ghostdrived`")) {
            line = "Start it on the Home page.";
        }
        replace_all(line, "ask for a new one with `ghostdrive invite`",
                    "ask for a new one (Add device, on a device of the drive)");
        replace_all(line, "run `ghostdrive init` or `ghostdrive join` first",
                    "create or join a drive first");
        replace_all(line, "`", "");
        if (!line.empty() && line[0] >= 'a' && line[0] <= 'z') line[0] = upper(line[0]);
        out.push_back(std::move(line));
    }
    if (out.empty())
        return "ghostdrive failed with exit status " + std::to_string(r.exit_status) + ".";
    return join_lines(out);
}

std::string plain_output(const CliResult& r, std::size_t max_lines) {
    return join_lines(clean_lines(r.out, max_lines, 400));
}

std::vector<std::string> cli_candidates(std::string_view app_dir, std::string_view path_env) {
    std::vector<std::string> out;
    if (!app_dir.empty() && app_dir[0] == '/') out.push_back(child(app_dir, "ghostdrive"));
    add_path_entries(out, path_env, "ghostdrive");
    return unique(std::move(out));
}

std::vector<std::string> daemon_candidates(std::string_view cli_path, std::string_view path_env) {
    std::vector<std::string> out;
    if (!cli_path.empty() && cli_path[0] == '/') {
        auto dir = parent_dir(cli_path);
        out.push_back(child(dir, "ghostdrived"));
        out.push_back(child(child(parent_dir(dir), "daemon"), "ghostdrived"));
    }
    add_path_entries(out, path_env, "ghostdrived");
    return unique(std::move(out));
}

AppSettings parse_settings(std::string_view text) {
    AppSettings out;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        auto eq = line.find('=');
        if (!line.empty() && line[0] != '#' && eq != std::string_view::npos) {
            auto key = trim(line.substr(0, eq));
            auto value = trim(line.substr(eq + 1));
            if (key == "data_dir") out.data_dir = value;
            if (key == "mountpoint") out.mountpoint = value;
        }
        start = end + 1;
    }
    return out;
}

std::string format_settings(const AppSettings& settings) {
    auto one_line = [](std::string v) {
        std::replace(v.begin(), v.end(), '\n', ' ');
        std::replace(v.begin(), v.end(), '\r', ' ');
        return trim(v);
    };
    return "# GhostDrive app settings; an empty value means the default.\n"
           "data_dir=" +
           one_line(settings.data_dir) + "\nmountpoint=" + one_line(settings.mountpoint) + "\n";
}

}  // namespace ghostdrive::app
