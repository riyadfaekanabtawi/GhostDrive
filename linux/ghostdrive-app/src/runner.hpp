#pragma once

#include <functional>
#include <optional>
#include <string>

#include "ghostdrive/app/cli.hpp"

namespace ghostdrive::app {

// The ghostdrive CLI next to this app or on PATH, or nullopt.
std::optional<std::string> find_cli();

// The ghostdrived for a CLI path (next to it, in a build tree, or on PATH), or nullopt.
std::optional<std::string> find_daemon(const std::string& cli);

// The directory holding this executable, or empty.
std::string app_dir();

// Runs `program args...` through Gio.Subprocess with an argument vector (no shell), stdin
// from /dev/null and "/" as the working directory. `done` runs on the main loop when the
// program has exited, or at once if it could not be started. A program still running after
// `timeout_seconds` is killed and reported as timed out.
void run_program(const std::string& program, const Argv& args, unsigned timeout_seconds,
                 std::function<void(CliResult)> done);

// Runs `ghostdrive args...`, finding the CLI first.
void run_cli(const Argv& args, unsigned timeout_seconds, std::function<void(CliResult)> done);

// Starts ghostdrived in its own session with its output appended to `log`; `exited` runs with
// its exit status (-1 when killed) when it exits. Returns an error message when it cannot be
// started.
std::optional<std::string> start_daemon(const Argv& args, const std::string& log,
                                        std::function<void(int status)> exited);

}  // namespace ghostdrive::app
