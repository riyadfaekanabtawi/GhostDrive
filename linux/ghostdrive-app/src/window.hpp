#pragma once

#include <adwaita.h>

#include <optional>
#include <string>
#include <vector>

#include "ghostdrive/app/cli.hpp"
#include "ghostdrive/app/status.hpp"

namespace ghostdrive::app {

struct Options {
    std::optional<std::string> data;  // --data: this session only, not saved
    std::string page;                 // --page: home, devices, add or settings
};

// The main window: first-run setup, then Home, Devices, Add device and Settings. Everything
// it knows comes from running the ghostdrive CLI; it never reads the drive's files.
class AppWindow {
public:
    AppWindow(AdwApplication* app, Options options);
    ~AppWindow();
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    void present();

private:
    GtkWidget* build_setup();
    GtkWidget* build_choose();
    GtkWidget* build_create();
    GtkWidget* build_join();
    GtkWidget* build_main();
    GtkWidget* build_home();
    GtkWidget* build_devices();
    GtkWidget* build_add();
    GtkWidget* build_settings();

    Paths paths() const;
    std::string data_dir() const;
    std::string mount_dir() const;
    bool drive_exists() const;
    void load_settings();
    void save_settings();

    void poll();
    void on_status(const CliResult& r);
    void show_root();
    void update_home();
    void update_devices();
    void update_run_controls();
    void refresh_settings();
    void refresh_id();

    void create_drive();
    void join_drive();
    void set_setup_busy(bool busy, const std::string& message, bool join);
    void setup_failed(const std::string& message, bool join);
    void setup_done(const std::string& text);

    void start_ghostdrive();
    void stop_ghostdrive();
    void confirm_remove(const Peer& peer);
    void remove_device(const Peer& peer);
    void create_invite();
    void tick_invite();
    void set_autostart(bool on);
    void save_rendezvous();
    void apply_folder(bool data);
    void open_drive();

    void toast(const std::string& text);
    void alert(const std::string& heading, const std::string& body);

    Options options_;
    AppSettings settings_;
    std::string settings_file_;
    std::string home_;

    bool alive_ = true;
    bool has_drive_ = false;
    bool shown_once_ = false;
    bool setup_busy_ = false;
    bool running_ = false;
    bool polling_ = false;
    bool starting_ = false;
    bool updating_ = false;
    bool invite_hint_ = false;
    gint64 starting_since_ = 0;
    std::optional<DriveStatus> status_;
    std::string error_;
    std::string id_fallback_;
    std::optional<AutostartState> autostart_;
    std::string invite_code_;
    gint64 invite_expires_ = 0;
    guint poll_timer_ = 0;
    guint invite_timer_ = 0;

    // Devices currently shown, to rebuild the list only when it changes.
    std::optional<std::vector<Peer>> shown_peers_;
    std::vector<GtkWidget*> device_rows_;

    GtkWindow* window_ = nullptr;
    AdwToastOverlay* toasts_ = nullptr;
    GtkStack* root_ = nullptr;
    GtkStack* setup_ = nullptr;
    AdwViewStack* views_ = nullptr;

    GtkWidget* create_name_ = nullptr;
    GtkWidget* create_button_ = nullptr;
    GtkWidget* create_back_ = nullptr;
    GtkWidget* create_spinner_ = nullptr;
    GtkWidget* create_message_ = nullptr;
    GtkWidget* join_name_ = nullptr;
    GtkWidget* join_code_ = nullptr;
    GtkWidget* join_button_ = nullptr;
    GtkWidget* join_back_ = nullptr;
    GtkWidget* join_spinner_ = nullptr;
    GtkWidget* join_message_ = nullptr;

    GtkWidget* banner_ = nullptr;
    GtkWidget* home_name_ = nullptr;
    GtkWidget* home_id_ = nullptr;
    GtkWidget* home_state_ = nullptr;
    GtkWidget* home_start_ = nullptr;
    GtkWidget* home_mount_ = nullptr;
    GtkWidget* open_button_ = nullptr;
    GtkWidget* home_rendezvous_ = nullptr;
    GtkWidget* home_discovery_ = nullptr;

    GtkWidget* devices_group_ = nullptr;

    GtkWidget* invite_button_ = nullptr;
    GtkWidget* copy_button_ = nullptr;
    GtkWidget* code_label_ = nullptr;
    GtkWidget* countdown_label_ = nullptr;
    GtkWidget* invite_message_ = nullptr;

    GtkWidget* data_entry_ = nullptr;
    GtkWidget* mount_entry_ = nullptr;
    GtkWidget* rv_current_ = nullptr;
    GtkWidget* rv_address_ = nullptr;
    GtkWidget* rv_fingerprint_ = nullptr;
    GtkWidget* rv_save_ = nullptr;
    GtkWidget* autostart_row_ = nullptr;
    GtkWidget* run_row_ = nullptr;
    GtkWidget* run_button_ = nullptr;
};

}  // namespace ghostdrive::app
