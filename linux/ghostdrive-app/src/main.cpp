// ghostdrive-app: the GhostDrive desktop app for Linux. It drives the ghostdrive CLI.

#include <adwaita.h>

#include <cstdio>
#include <memory>
#include <string_view>

#include "runner.hpp"
#include "window.hpp"

namespace {

using ghostdrive::app::AppWindow;
using ghostdrive::app::Options;

constexpr const char* kUsage =
    "usage: ghostdrive-app [--data DIR] [--page home|devices|add|settings]\n"
    "  --data DIR   use this data folder for this session (the setting is not changed)\n"
    "  --page NAME  open on this page\n";

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if ((a == "--data" || a == "--page") && i + 1 < argc) {
            std::string_view v = argv[++i];
            if (a == "--data") {
                if (v.empty() || v[0] != '/') return false;
                out.data = std::string(v);
            } else {
                if (v != "home" && v != "devices" && v != "add" && v != "settings") return false;
                out.page = std::string(v);
            }
        } else {
            return false;
        }
    }
    return true;
}

struct App {
    Options options;
    std::unique_ptr<AppWindow> window;
};

void on_activate(GApplication* application, gpointer data) {
    auto* app = static_cast<App*>(data);
    if (!app->window) {
        // The icon next to the binary in a build tree; installed copies are found by the theme.
        std::string icons = ghostdrive::app::app_dir() + "/icons";
        gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()),
                                       icons.c_str());
        app->window = std::make_unique<AppWindow>(ADW_APPLICATION(application), app->options);
    }
    app->window->present();
}

}  // namespace

int main(int argc, char** argv) {
    App app;
    if (!parse_args(argc, argv, app.options)) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    AdwApplication* application =
        adw_application_new("org.ghostdrive.GhostDrive", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), &app);
    // The options are handled above, so GApplication sees only the program name.
    int status = g_application_run(G_APPLICATION(application), 1, argv);
    app.window.reset();
    g_object_unref(application);
    return status;
}
