#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "ghostdrive/core/result.hpp"

// Files that start ghostdrived at login: a launchd LaunchAgent on macOS, a systemd user unit on
// Linux. Everything here is pure except write_file_atomic; running launchctl or systemctl is the
// CLI's job.
namespace ghostdrive::autostart {

struct Spec {
    std::filesystem::path daemon;  // absolute path of ghostdrived
    std::filesystem::path data;    // absolute data directory
    std::filesystem::path mount;   // absolute mountpoint
    std::filesystem::path log;     // absolute log file (launchd only; systemd uses the journal)
};

// 16 hex digits identifying a data directory, so each drive on a machine gets its own job.
// `data` should already be absolute and normalised.
std::string data_dir_id(const std::filesystem::path& data);

std::string launchd_label(const std::filesystem::path& data);      // io.ghostdrive.daemon.<id>
std::string systemd_unit_name(const std::filesystem::path& data);  // ghostdrive-<id>.service

// ~/Library/LaunchAgents/<label>.plist
std::filesystem::path launch_agent_path(const std::filesystem::path& home,
                                        const std::string& label);
// $XDG_CONFIG_HOME/systemd/user/<unit>, where config_home is $XDG_CONFIG_HOME or ~/.config.
std::filesystem::path systemd_unit_path(const std::filesystem::path& config_home,
                                        const std::string& unit);

// Escapes text for XML character data and attribute values. Fails on control characters,
// which XML 1.0 cannot carry and no path of ours should contain.
core::Result<std::string> xml_escape(std::string_view text);
// One argument of a systemd command line as a double-quoted word, with backslash, quote,
// specifier (%) and variable ($) characters escaped. Fails on control characters.
core::Result<std::string> systemd_quote(std::string_view text);

core::Result<std::string> launch_agent_plist(const std::string& label, const Spec& spec);
core::Result<std::string> systemd_unit(const Spec& spec);

// Writes `text` to `path` with mode 0644 through a temp file in the same directory and a
// rename, creating the parent directories.
core::Status write_file_atomic(const std::filesystem::path& path, std::string_view text);

}  // namespace ghostdrive::autostart
