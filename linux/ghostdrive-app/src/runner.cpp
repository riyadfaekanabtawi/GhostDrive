#include "runner.hpp"

#include <fcntl.h>
#include <gio/gio.h>
#include <unistd.h>

#include <memory>
#include <vector>

namespace ghostdrive::app {

namespace {

struct Running {
    GSubprocess* process = nullptr;
    guint timer = 0;
    bool timed_out = false;
    std::function<void(CliResult)> done;
};

std::string to_string(GBytes* bytes) {
    if (!bytes) return {};
    gsize size = 0;
    const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    return data ? std::string(data, size) : std::string();
}

std::vector<const char*> c_argv(const std::string& program, const Argv& args) {
    std::vector<const char*> argv{program.c_str()};
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    return argv;
}

bool is_program(const std::string& path) {
    return g_file_test(path.c_str(), G_FILE_TEST_IS_EXECUTABLE) &&
           !g_file_test(path.c_str(), G_FILE_TEST_IS_DIR);
}

std::string path_env() {
    const char* path = g_getenv("PATH");
    return path ? path : "";
}

gboolean on_timeout(gpointer data) {
    auto* running = static_cast<Running*>(data);
    running->timer = 0;
    running->timed_out = true;
    g_subprocess_force_exit(running->process);
    return G_SOURCE_REMOVE;
}

void on_communicated(GObject* source, GAsyncResult* result, gpointer data) {
    // Takes back the state run_program handed to the main loop.
    std::unique_ptr<Running> running(static_cast<Running*>(data));
    if (running->timer) g_source_remove(running->timer);
    GBytes* out = nullptr;
    GBytes* err = nullptr;
    GError* error = nullptr;
    CliResult r;
    r.launched = true;
    r.timed_out = running->timed_out;
    if (g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, &err, &error)) {
        r.out = to_string(out);
        r.err = to_string(err);
        if (g_subprocess_get_if_exited(running->process)) {
            r.exit_status = g_subprocess_get_exit_status(running->process);
        }
    } else {
        r.err = error ? error->message : "could not read the program's output";
        g_clear_error(&error);
    }
    if (out) g_bytes_unref(out);
    if (err) g_bytes_unref(err);
    g_object_unref(running->process);
    running->done(std::move(r));
}

struct Watch {
    std::function<void(int)> exited;
};

void on_daemon_exit(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<Watch> watch(static_cast<Watch*>(data));
    auto* process = G_SUBPROCESS(source);
    int status = -1;
    if (g_subprocess_wait_finish(process, result, nullptr) && g_subprocess_get_if_exited(process)) {
        status = g_subprocess_get_exit_status(process);
    }
    g_object_unref(process);
    watch->exited(status);
}

}  // namespace

std::string app_dir() {
    gchar* exe = g_file_read_link("/proc/self/exe", nullptr);
    if (!exe) return {};
    gchar* dir = g_path_get_dirname(exe);
    std::string out = dir;
    g_free(dir);
    g_free(exe);
    return out;
}

std::optional<std::string> find_cli() {
    for (const auto& candidate : cli_candidates(app_dir(), path_env())) {
        if (is_program(candidate)) return candidate;
    }
    return std::nullopt;
}

std::optional<std::string> find_daemon(const std::string& cli) {
    for (const auto& candidate : daemon_candidates(cli, path_env())) {
        if (is_program(candidate)) return candidate;
    }
    return std::nullopt;
}

void run_program(const std::string& program, const Argv& args, unsigned timeout_seconds,
                 std::function<void(CliResult)> done) {
    auto argv = c_argv(program, args);
    GSubprocessLauncher* launcher = g_subprocess_launcher_new(static_cast<GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_subprocess_launcher_set_cwd(launcher, "/");
    GError* error = nullptr;
    GSubprocess* process = g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
    g_object_unref(launcher);
    if (!process) {
        CliResult r;
        r.err = error ? error->message : "could not start " + program;
        g_clear_error(&error);
        done(std::move(r));
        return;
    }
    auto running = std::make_unique<Running>();
    running->process = process;
    running->done = std::move(done);
    // Owned by the main loop until on_communicated takes it back.
    Running* raw = running.release();
    raw->timer = g_timeout_add_seconds(timeout_seconds, on_timeout, raw);
    g_subprocess_communicate_async(process, nullptr, nullptr, on_communicated, raw);
}

void run_cli(const Argv& args, unsigned timeout_seconds, std::function<void(CliResult)> done) {
    auto cli = find_cli();
    if (!cli) {
        CliResult r;
        r.err = "ghostdrive was not found next to this app or on PATH";
        done(std::move(r));
        return;
    }
    run_program(*cli, args, timeout_seconds, std::move(done));
}

std::optional<std::string> start_daemon(const Argv& args, const std::string& log,
                                        std::function<void(int status)> exited) {
    auto cli = find_cli();
    if (!cli) return "Could not find the ghostdrive tool next to this app or on PATH.";
    auto daemon = find_daemon(*cli);
    if (!daemon) return "Could not find ghostdrived next to " + *cli + " or on PATH.";

    int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return "Could not open the log file " + log + ".";
    GSubprocessLauncher* launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_set_cwd(launcher, "/");
    g_subprocess_launcher_take_stdout_fd(launcher, fd);
    // Its own session, so it outlives a terminal the app was started from.
    g_subprocess_launcher_set_child_setup(launcher, [](gpointer) { ::setsid(); }, nullptr, nullptr);
    auto argv = c_argv(*daemon, args);
    GError* error = nullptr;
    GSubprocess* process = g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
    g_object_unref(launcher);
    if (!process) {
        std::string message = "Could not start ghostdrived: ";
        message += error ? error->message : "unknown error";
        g_clear_error(&error);
        return message;
    }
    auto watch = std::make_unique<Watch>();
    watch->exited = std::move(exited);
    g_subprocess_wait_async(process, nullptr, on_daemon_exit, watch.release());
    return std::nullopt;
}

}  // namespace ghostdrive::app
