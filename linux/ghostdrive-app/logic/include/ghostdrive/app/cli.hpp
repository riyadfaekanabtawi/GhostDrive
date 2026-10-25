#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ghostdrive::app {

using Argv = std::vector<std::string>;

// The data folder and mountpoint passed to the CLI. nullopt means the CLI's own default.
struct Paths {
    std::optional<std::string> data;
    std::optional<std::string> mount;
};

// The argument vectors run as `ghostdrive <args>` (cli/ghostdrive.cpp), without the program.
// The app never builds a shell command line.
namespace commands {
Argv status(const Paths& p);
Argv id(const Paths& p);
Argv init(const Paths& p, const std::string& name);
Argv join(const Paths& p, const std::string& code, const std::string& name);
Argv invite(const Paths& p, long ttl_seconds);
Argv remove(const Paths& p, const std::string& id);
Argv stop(const Paths& p);
Argv autostart_on(const Paths& p);
Argv autostart_off(const Paths& p);
Argv autostart_status(const Paths& p);
Argv rendezvous_show(const Paths& p);
Argv rendezvous_set(const Paths& p, const std::string& address, const std::string& fingerprint);
// Arguments for `ghostdrived` itself, which Start runs directly.
Argv daemon(const Paths& p);
}  // namespace commands

// Checks of what the user typed. Each returns the problem in plain words, or nullopt.
std::optional<std::string> device_name_problem(std::string_view name);
std::optional<std::string> path_problem(std::string_view value);
std::optional<std::string> rendezvous_address_problem(std::string_view address);

// The invite code in pasted text (which may be the whole "ghostdrive join gd1-..." line, wrapped
// or in lower case), upper-cased; nullopt if there is none.
std::optional<std::string> extract_invite_code(std::string_view text);

// "gd1-" and dash-separated groups of base32 (daemon/src/invite.cpp).
bool is_invite_code(std::string_view word);

// A device or server fingerprint as typed (spaces or dashes anywhere, any case), in the CLI's
// form of 52 base32 characters in dash-separated groups of four; nullopt if it is not one.
std::optional<std::string> normalize_fingerprint(std::string_view text);

// A settings value as a path: trimmed, a leading "~" expanded with `home`; nullopt when empty.
std::optional<std::string> expand_path(std::string_view value, std::string_view home);

// The CLI's defaults on Linux (daemon/src/drive.cpp).
std::string default_data_dir(std::string_view home, std::string_view xdg_data_home);
std::string default_mountpoint(std::string_view home);

// The file whose presence means a drive exists in a data folder. The app only checks that it
// exists; it never opens it.
std::string drive_marker(std::string_view data_dir);

// True when /proc/self/mountinfo text lists `path` as a mount point. Reading the table, rather
// than calling stat on the mountpoint, cannot block on a FUSE daemon that has stopped answering.
bool mountinfo_has(std::string_view mountinfo, std::string_view path);

// What `ghostdrive invite` printed.
struct Invite {
    std::string code;
    std::optional<long> minutes;
};
std::optional<Invite> parse_invite(std::string_view text);

// "9:05" for 545 seconds; "0:00" for zero or less.
std::string format_countdown(long seconds);

// What `ghostdrive autostart status` printed.
struct AutostartState {
    bool configured = false;  // a login item exists
    bool active = false;      // systemd reports it active (launchd: loaded)
};
std::optional<AutostartState> parse_autostart_status(std::string_view text);

// What `ghostdrive rendezvous` printed: "none" or "HOST:PORT FINGERPRINT".
struct RendezvousSetting {
    bool configured = false;
    std::string address;
    std::string fingerprint;
};
std::optional<RendezvousSetting> parse_rendezvous_setting(std::string_view text);

// The result of running the CLI.
struct CliResult {
    bool launched = false;  // false when the program could not be started at all
    bool timed_out = false;
    int exit_status = -1;
    std::string out;
    std::string err;

    bool ok() const { return launched && !timed_out && exit_status == 0; }
};

// True when a failed command says the daemon is not running (as opposed to a real error).
bool says_not_running(const CliResult& r);

// The CLI's error output in plain words: cleaned lines without the "ghostdrive: " prefix, with
// terminal instructions replaced by what to do in the app.
std::string plain_error(const CliResult& r);

// A successful command's output, cleaned, for showing as is (for example `remove`, whose
// output names devices the removed one added).
std::string plain_output(const CliResult& r, std::size_t max_lines = 12);

// Where to look for the CLI: next to the app, then each absolute PATH entry.
std::vector<std::string> cli_candidates(std::string_view app_dir, std::string_view path_env);

// Where to look for ghostdrived: next to the CLI, the build tree's daemon/ beside cli/, then
// each absolute PATH entry.
std::vector<std::string> daemon_candidates(std::string_view cli_path, std::string_view path_env);

// The app's own settings file (key=value lines). Empty values mean the defaults.
struct AppSettings {
    std::string data_dir;
    std::string mountpoint;

    bool operator==(const AppSettings&) const = default;
};
AppSettings parse_settings(std::string_view text);
std::string format_settings(const AppSettings& settings);

}  // namespace ghostdrive::app
