#include "window.hpp"

#include <functional>
#include <memory>

#include "ghostdrive/app/text.hpp"
#include "runner.hpp"

namespace ghostdrive::app {

namespace {

constexpr long kInviteSeconds = 600;
constexpr guint kPollSeconds = 3;

// Signal handlers as std::function, freed with the signal connection.
using Action = std::function<void()>;
using NamedAction = std::function<void(const std::string&)>;

template <typename F>
void free_handler(gpointer data, GClosure*) {
    std::unique_ptr<F> owned(static_cast<F*>(data));
}

// For signals with no arguments besides the instance ("clicked", "apply", "activate").
void on(gpointer instance, const char* signal, Action action) {
    auto handler = std::make_unique<Action>(std::move(action));
    g_signal_connect_data(instance, signal, G_CALLBACK(+[](gpointer, gpointer data) {
                              (*static_cast<Action*>(data))();
                          }),
                          handler.release(), free_handler<Action>, static_cast<GConnectFlags>(0));
}

// For "notify::property" signals.
void on_notify(gpointer instance, const char* signal, Action action) {
    auto handler = std::make_unique<Action>(std::move(action));
    g_signal_connect_data(instance, signal, G_CALLBACK(+[](gpointer, GParamSpec*, gpointer data) {
                              (*static_cast<Action*>(data))();
                          }),
                          handler.release(), free_handler<Action>, static_cast<GConnectFlags>(0));
}

// For AdwAlertDialog "response".
void on_response(gpointer instance, NamedAction action) {
    auto handler = std::make_unique<NamedAction>(std::move(action));
    g_signal_connect_data(
        instance, "response", G_CALLBACK(+[](gpointer, const char* response, gpointer data) {
            (*static_cast<NamedAction*>(data))(response ? response : "");
        }),
        handler.release(), free_handler<NamedAction>, static_cast<GConnectFlags>(0));
}

// Rows never interpret their text as Pango markup: titles and subtitles carry daemon text.
GtkWidget* action_row(const char* title, const std::string& subtitle = "") {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
    return row;
}

void set_subtitle(GtkWidget* row, const std::string& subtitle) {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
}

GtkWidget* entry_row(const char* title) {
    GtkWidget* row = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    return row;
}

std::string entry_text(GtkWidget* entry) {
    const char* text = gtk_editable_get_text(GTK_EDITABLE(entry));
    return text ? text : "";
}

GtkWidget* button(const char* label, const char* style = nullptr) {
    GtkWidget* b = gtk_button_new_with_label(label);
    if (style) gtk_widget_add_css_class(b, style);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

GtkWidget* pill(const char* label, bool suggested) {
    GtkWidget* b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "pill");
    if (suggested) gtk_widget_add_css_class(b, "suggested-action");
    gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
    return b;
}

GtkWidget* label(const std::string& text, const char* style = nullptr) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    if (style) gtk_widget_add_css_class(l, style);
    return l;
}

GtkWidget* vbox(int spacing) {
    return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
}

GtkWidget* clamp(GtkWidget* child) {
    GtkWidget* c = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(c), 520);
    adw_clamp_set_child(ADW_CLAMP(c), child);
    gtk_widget_set_margin_top(c, 24);
    gtk_widget_set_margin_bottom(c, 24);
    gtk_widget_set_margin_start(c, 12);
    gtk_widget_set_margin_end(c, 12);
    return c;
}

GtkWidget* scrolled(GtkWidget* child) {
    GtkWidget* s = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s), child);
    gtk_widget_set_vexpand(s, TRUE);
    return s;
}

std::string host_name() {
    std::string name = g_get_host_name();
    if (auto dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    return name == "localhost" ? std::string() : clean_text(name);
}

std::string env(const char* name) {
    const char* v = g_getenv(name);
    return v ? v : "";
}

std::string read_file(const std::string& path) {
    gchar* contents = nullptr;
    gsize size = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &size, nullptr)) return {};
    std::string out(contents, size);
    g_free(contents);
    return out;
}

// The last lines of the daemon log, to explain why it stopped.
std::string log_tail(const std::string& path) {
    std::string text = read_file(path);
    if (text.size() > 2000) text = text.substr(text.size() - 2000);
    auto lines = clean_lines(text, 200, 160);
    if (lines.size() > 4) lines.erase(lines.begin(), lines.end() - 4);
    return join_lines(lines);
}

}  // namespace

AppWindow::AppWindow(AdwApplication* app, Options options) : options_(std::move(options)) {
    home_ = g_get_home_dir();
    gchar* file = g_build_filename(g_get_user_config_dir(), "ghostdrive", "app.conf", nullptr);
    settings_file_ = file;
    g_free(file);
    load_settings();

    GtkWidget* window = adw_application_window_new(GTK_APPLICATION(app));
    window_ = GTK_WINDOW(window);
    gtk_window_set_title(window_, "GhostDrive");
    gtk_window_set_icon_name(window_, "org.ghostdrive.GhostDrive");
    gtk_window_set_default_size(window_, 760, 620);
    gtk_widget_set_size_request(window, 360, 400);

    toasts_ = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    root_ = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(root_, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(root_, build_setup(), "setup");
    gtk_stack_add_named(root_, build_main(), "main");
    adw_toast_overlay_set_child(toasts_, GTK_WIDGET(root_));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), GTK_WIDGET(toasts_));

    // Narrow windows move the page switcher to the bottom.
    AdwBreakpoint* narrow = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 560sp"));
    GtkWidget* bar = GTK_WIDGET(g_object_get_data(G_OBJECT(root_), "switcher-bar"));
    GtkWidget* switcher = GTK_WIDGET(g_object_get_data(G_OBJECT(root_), "switcher"));
    adw_breakpoint_add_setters(narrow, G_OBJECT(bar), "reveal", TRUE, G_OBJECT(switcher), "visible",
                               FALSE, nullptr);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), narrow);

    on(window, "destroy", [this] {
        alive_ = false;
        if (poll_timer_) g_source_remove(poll_timer_);
        if (invite_timer_) g_source_remove(invite_timer_);
        poll_timer_ = invite_timer_ = 0;
    });

    has_drive_ = drive_exists();
    show_root();
    if (!options_.page.empty())
        adw_view_stack_set_visible_child_name(views_, options_.page.c_str());
    poll();
    refresh_settings();
    poll_timer_ = g_timeout_add_seconds(
        kPollSeconds,
        [](gpointer self) -> gboolean {
            static_cast<AppWindow*>(self)->poll();
            return G_SOURCE_CONTINUE;
        },
        this);
}

AppWindow::~AppWindow() {
    if (alive_) {
        if (poll_timer_) g_source_remove(poll_timer_);
        if (invite_timer_) g_source_remove(invite_timer_);
    }
}

void AppWindow::present() {
    gtk_window_present(window_);
}

// ---- Building the pages ----

GtkWidget* AppWindow::build_setup() {
    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
    setup_ = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(setup_, GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_add_named(setup_, build_choose(), "choose");
    gtk_stack_add_named(setup_, scrolled(clamp(build_create())), "create");
    gtk_stack_add_named(setup_, scrolled(clamp(build_join())), "join");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(setup_));
    return view;
}

GtkWidget* AppWindow::build_choose() {
    GtkWidget* page = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "org.ghostdrive.GhostDrive");
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "Welcome to GhostDrive");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(page),
        "A drive made of your own devices. Files stay on your devices and travel between them "
        "end-to-end encrypted.");
    GtkWidget* box = vbox(12);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    GtkWidget* create = pill("Create a new drive", true);
    GtkWidget* join = pill("Join with an invite code", false);
    gtk_box_append(GTK_BOX(box), create);
    gtk_box_append(GTK_BOX(box), join);
    adw_status_page_set_child(ADW_STATUS_PAGE(page), box);
    on(create, "clicked", [this] {
        gtk_stack_set_visible_child_name(setup_, "create");
        gtk_widget_grab_focus(create_name_);
    });
    on(join, "clicked", [this] {
        gtk_stack_set_visible_child_name(setup_, "join");
        gtk_widget_grab_focus(join_code_);
    });
    return page;
}

GtkWidget* AppWindow::build_create() {
    GtkWidget* box = vbox(18);
    gtk_box_append(GTK_BOX(box), label("Create a new drive", "title-2"));
    gtk_box_append(GTK_BOX(box),
                   label("This device becomes the first device of a new drive. You can add your "
                         "other devices afterwards.",
                         "dim-label"));
    GtkWidget* group = adw_preferences_group_new();
    create_name_ = entry_row("Device name");
    gtk_editable_set_text(GTK_EDITABLE(create_name_), host_name().c_str());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create_name_);
    gtk_box_append(GTK_BOX(box), group);

    GtkWidget* status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    create_spinner_ = gtk_spinner_new();
    create_message_ = label("");
    gtk_widget_set_hexpand(create_message_, TRUE);
    gtk_box_append(GTK_BOX(status), create_spinner_);
    gtk_box_append(GTK_BOX(status), create_message_);
    gtk_box_append(GTK_BOX(box), status);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    create_back_ = button("Back");
    create_button_ = button("Create drive", "suggested-action");
    gtk_box_append(GTK_BOX(buttons), create_back_);
    gtk_box_append(GTK_BOX(buttons), create_button_);
    gtk_box_append(GTK_BOX(box), buttons);

    on(create_back_, "clicked", [this] { gtk_stack_set_visible_child_name(setup_, "choose"); });
    on(create_button_, "clicked", [this] { create_drive(); });
    on(create_name_, "entry-activated", [this] { create_drive(); });
    return box;
}

GtkWidget* AppWindow::build_join() {
    GtkWidget* box = vbox(18);
    gtk_box_append(GTK_BOX(box), label("Join with an invite code", "title-2"));
    gtk_box_append(GTK_BOX(box),
                   label("On a device that is already part of the drive, open GhostDrive and "
                         "choose Add device. Paste the code it shows here. Both devices need to "
                         "be on the same network while this one joins.",
                         "dim-label"));
    GtkWidget* group = adw_preferences_group_new();
    join_code_ = entry_row("Invite code");
    join_name_ = entry_row("Device name");
    gtk_editable_set_text(GTK_EDITABLE(join_name_), host_name().c_str());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), join_code_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), join_name_);
    gtk_box_append(GTK_BOX(box), group);

    GtkWidget* status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    join_spinner_ = gtk_spinner_new();
    join_message_ = label("");
    gtk_widget_set_hexpand(join_message_, TRUE);
    gtk_box_append(GTK_BOX(status), join_spinner_);
    gtk_box_append(GTK_BOX(status), join_message_);
    gtk_box_append(GTK_BOX(box), status);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    join_back_ = button("Back");
    join_button_ = button("Join drive", "suggested-action");
    gtk_box_append(GTK_BOX(buttons), join_back_);
    gtk_box_append(GTK_BOX(buttons), join_button_);
    gtk_box_append(GTK_BOX(box), buttons);

    on(join_back_, "clicked", [this] { gtk_stack_set_visible_child_name(setup_, "choose"); });
    on(join_button_, "clicked", [this] { join_drive(); });
    on(join_name_, "entry-activated", [this] { join_drive(); });
    return box;
}

GtkWidget* AppWindow::build_main() {
    GtkWidget* view = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    views_ = ADW_VIEW_STACK(adw_view_stack_new());
    GtkWidget* switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), views_);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    adw_view_stack_add_titled_with_icon(views_, build_home(), "home", "Home", "user-home-symbolic");
    adw_view_stack_add_titled_with_icon(views_, build_devices(), "devices", "Devices",
                                        "computer-symbolic");
    adw_view_stack_add_titled_with_icon(views_, build_add(), "add", "Add device",
                                        "list-add-symbolic");
    adw_view_stack_add_titled_with_icon(views_, build_settings(), "settings", "Settings",
                                        "emblem-system-symbolic");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(views_));

    GtkWidget* bar = adw_view_switcher_bar_new();
    adw_view_switcher_bar_set_stack(ADW_VIEW_SWITCHER_BAR(bar), views_);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), bar);
    g_object_set_data(G_OBJECT(root_), "switcher-bar", bar);
    g_object_set_data(G_OBJECT(root_), "switcher", switcher);

    on_notify(views_, "notify::visible-child", [this] {
        const char* name = adw_view_stack_get_visible_child_name(views_);
        if (name && std::string_view(name) == "settings") refresh_settings();
    });
    return view;
}

GtkWidget* AppWindow::build_home() {
    GtkWidget* box = vbox(0);
    banner_ = adw_banner_new("");
    adw_banner_set_use_markup(ADW_BANNER(banner_), FALSE);
    gtk_box_append(GTK_BOX(box), banner_);

    GtkWidget* page = adw_preferences_page_new();
    gtk_widget_set_vexpand(page, TRUE);
    GtkWidget* device = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(device), "This device");
    home_name_ = action_row("Name");
    home_id_ = action_row("Device ID");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(home_id_), TRUE);
    home_state_ = action_row("GhostDrive");
    home_start_ = button("Start", "suggested-action");
    adw_action_row_add_suffix(ADW_ACTION_ROW(home_state_), home_start_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(device), home_name_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(device), home_id_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(device), home_state_);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(device));

    GtkWidget* drive = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(drive), "Drive");
    home_mount_ = action_row("Mount location");
    open_button_ = button("Open drive");
    adw_action_row_add_suffix(ADW_ACTION_ROW(home_mount_), open_button_);
    home_rendezvous_ = action_row("Rendezvous server");
    home_discovery_ = action_row("Discovery");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(drive), home_mount_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(drive), home_rendezvous_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(drive), home_discovery_);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(drive));
    gtk_box_append(GTK_BOX(box), page);

    on(home_start_, "clicked", [this] { start_ghostdrive(); });
    on(open_button_, "clicked", [this] { open_drive(); });
    return box;
}

GtkWidget* AppWindow::build_devices() {
    GtkWidget* page = adw_preferences_page_new();
    devices_group_ = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(devices_group_), "Other devices");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(devices_group_),
        "The devices in this drive besides this one. Remove a device that is lost or stolen.");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(devices_group_));
    return page;
}

GtkWidget* AppWindow::build_add() {
    GtkWidget* page = adw_preferences_page_new();
    GtkWidget* group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Invite a device");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "An invite code lets one more device join this drive. It works once, for 10 minutes.");

    GtkWidget* box = vbox(12);
    gtk_widget_set_margin_top(box, 6);
    code_label_ = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(code_label_), TRUE);
    gtk_label_set_wrap(GTK_LABEL(code_label_), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(code_label_), PANGO_WRAP_CHAR);
    gtk_label_set_justify(GTK_LABEL(code_label_), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(code_label_, "title-1");
    gtk_widget_add_css_class(code_label_, "monospace");
    gtk_widget_set_visible(code_label_, FALSE);
    countdown_label_ = gtk_label_new("");
    gtk_widget_add_css_class(countdown_label_, "dim-label");
    gtk_widget_set_visible(countdown_label_, FALSE);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
    invite_button_ = pill("Create invite code", true);
    copy_button_ = pill("Copy code", false);
    gtk_widget_set_visible(copy_button_, FALSE);
    gtk_box_append(GTK_BOX(buttons), invite_button_);
    gtk_box_append(GTK_BOX(buttons), copy_button_);
    invite_message_ = label("");
    gtk_label_set_xalign(GTK_LABEL(invite_message_), 0.5f);
    gtk_label_set_justify(GTK_LABEL(invite_message_), GTK_JUSTIFY_CENTER);
    gtk_widget_set_visible(invite_message_, FALSE);

    gtk_box_append(GTK_BOX(box), code_label_);
    gtk_box_append(GTK_BOX(box), countdown_label_);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_box_append(GTK_BOX(box), invite_message_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), box);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));

    GtkWidget* steps = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(steps), "On the other device");
    const char* text[] = {
        "Install GhostDrive and open it.",
        "Choose Join with an invite code, then paste this code and give the device a name.",
        "Keep both devices on the same network until it has joined.",
    };
    for (int i = 0; i < 3; ++i) {
        GtkWidget* row = action_row(text[i]);
        GtkWidget* number = gtk_label_new(std::to_string(i + 1).c_str());
        gtk_widget_add_css_class(number, "dim-label");
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), number);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(steps), row);
    }
    GtkWidget* warning = action_row("Anyone who has the code can join, so send it privately.");
    adw_action_row_add_prefix(ADW_ACTION_ROW(warning),
                              gtk_image_new_from_icon_name("dialog-warning-symbolic"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(steps), warning);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(steps));

    on(invite_button_, "clicked", [this] { create_invite(); });
    on(copy_button_, "clicked", [this] {
        if (invite_code_.empty()) return;
        gdk_clipboard_set_text(gtk_widget_get_clipboard(code_label_), invite_code_.c_str());
        toast("Invite code copied");
    });
    return page;
}

GtkWidget* AppWindow::build_settings() {
    GtkWidget* page = adw_preferences_page_new();

    GtkWidget* run = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(run), "GhostDrive");
    run_row_ = action_row("GhostDrive");
    run_button_ = button("Start");
    adw_action_row_add_suffix(ADW_ACTION_ROW(run_row_), run_button_);
    autostart_row_ = adw_switch_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(autostart_row_), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(autostart_row_), "Start at login");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(autostart_row_),
                                "A systemd user service. Turning it off also stops GhostDrive.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(run), run_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(run), autostart_row_);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(run));

    GtkWidget* folders = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(folders), "Folders");
    std::string defaults = "Leave a field empty for the default: data in " +
                           default_data_dir("~", env("XDG_DATA_HOME")) +
                           ", the drive at ~/GhostDrive. Press Enter or the check mark to save.";
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(folders), defaults.c_str());
    data_entry_ = entry_row("Data folder");
    mount_entry_ = entry_row("Mount location");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(data_entry_), TRUE);
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(mount_entry_), TRUE);
    gtk_editable_set_text(GTK_EDITABLE(data_entry_),
                          (options_.data ? *options_.data : settings_.data_dir).c_str());
    gtk_editable_set_text(GTK_EDITABLE(mount_entry_), settings_.mountpoint.c_str());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(folders), data_entry_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(folders), mount_entry_);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(folders));

    GtkWidget* rv = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(rv), "Rendezvous server");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(rv),
        "Lets devices on different networks find each other. Run ghostdrive-rendezvous on a "
        "machine with a public IP; it prints the fingerprint to enter here. It only ever sees "
        "encrypted traffic.");
    rv_current_ = action_row("Current server", "Unknown");
    rv_address_ = entry_row("Address (host:port)");
    rv_fingerprint_ = entry_row("Server fingerprint");
    rv_save_ = button("Set server", "suggested-action");
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(rv), rv_save_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(rv), rv_current_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(rv), rv_address_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(rv), rv_fingerprint_);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(rv));

    on(run_button_, "clicked", [this] {
        if (running_) {
            stop_ghostdrive();
        } else {
            start_ghostdrive();
        }
    });
    on_notify(autostart_row_, "notify::active", [this] {
        if (!updating_) set_autostart(adw_switch_row_get_active(ADW_SWITCH_ROW(autostart_row_)));
    });
    on(data_entry_, "apply", [this] { apply_folder(true); });
    on(mount_entry_, "apply", [this] { apply_folder(false); });
    on(rv_save_, "clicked", [this] { save_rendezvous(); });
    return page;
}

// ---- Settings and paths ----

void AppWindow::load_settings() {
    settings_ = parse_settings(read_file(settings_file_));
}

void AppWindow::save_settings() {
    gchar* dir = g_path_get_dirname(settings_file_.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    std::string text = format_settings(settings_);
    GError* error = nullptr;
    if (!g_file_set_contents(settings_file_.c_str(), text.c_str(), static_cast<gssize>(text.size()),
                             &error)) {
        alert("Could not save the settings", clean_text(error ? error->message : "", 200));
        g_clear_error(&error);
    }
}

Paths AppWindow::paths() const {
    Paths p;
    std::string data = options_.data ? *options_.data : settings_.data_dir;
    if (!path_problem(data)) p.data = expand_path(data, home_);
    if (!path_problem(settings_.mountpoint)) p.mount = expand_path(settings_.mountpoint, home_);
    return p;
}

std::string AppWindow::data_dir() const {
    auto p = paths();
    return p.data ? *p.data : default_data_dir(home_, env("XDG_DATA_HOME"));
}

std::string AppWindow::mount_dir() const {
    auto p = paths();
    return p.mount ? *p.mount : default_mountpoint(home_);
}

bool AppWindow::drive_exists() const {
    return g_file_test(drive_marker(data_dir()).c_str(), G_FILE_TEST_EXISTS);
}

// ---- Status ----

void AppWindow::show_root() {
    gtk_stack_set_visible_child_name(root_, has_drive_ ? "main" : "setup");
    if (!has_drive_) gtk_stack_set_visible_child_name(setup_, "choose");
}

void AppWindow::poll() {
    if (!alive_ || polling_ || setup_busy_) return;
    bool exists = drive_exists();
    if (exists != has_drive_) {
        has_drive_ = exists;
        status_.reset();
        running_ = false;
        id_fallback_.clear();
        show_root();
    }
    if (!has_drive_) return;
    polling_ = true;
    run_cli(commands::status(paths()), 10, [this](CliResult r) {
        polling_ = false;
        if (alive_) on_status(r);
    });
}

void AppWindow::on_status(const CliResult& r) {
    error_.clear();
    if (r.ok()) {
        status_ = parse_status(r.out);
        running_ = true;
        starting_ = false;
        if (!status_) error_ = "GhostDrive answered with a status this app does not understand.";
    } else {
        status_.reset();
        running_ = false;
        if (!says_not_running(r)) error_ = plain_error(r);
        // Give up waiting for a daemon that was started but never answered.
        if (starting_ && g_get_monotonic_time() - starting_since_ > 15 * G_USEC_PER_SEC) {
            starting_ = false;
        }
        if (id_fallback_.empty()) refresh_id();
    }
    update_home();
    update_devices();
    update_run_controls();
    shown_once_ = true;
}

void AppWindow::refresh_id() {
    run_cli(commands::id(paths()), 10, [this](CliResult r) {
        if (!alive_ || !r.ok()) return;
        auto lines = clean_lines(r.out, 1, 128);
        if (!lines.empty()) id_fallback_ = lines[0];
        update_home();
    });
}

void AppWindow::update_home() {
    const bool removed = status_ && status_->removed_by;
    if (removed) {
        std::string text = "This device was removed from the drive by " + *status_->removed_by +
                           ". It can no longer reach the other devices.";
        adw_banner_set_title(ADW_BANNER(banner_), text.c_str());
    }
    adw_banner_set_revealed(ADW_BANNER(banner_), removed);

    set_subtitle(home_name_, status_ ? status_->device : "Shown while GhostDrive is running");
    set_subtitle(home_id_,
                 status_ ? status_->id : (id_fallback_.empty() ? "Unknown" : id_fallback_));
    if (running_) {
        set_subtitle(home_state_, "Running");
    } else if (!error_.empty()) {
        set_subtitle(home_state_, "Not running: " + error_);
    } else {
        set_subtitle(home_state_, starting_ ? "Starting..." : "Not running");
    }
    gtk_widget_set_visible(home_start_, !running_);
    gtk_widget_set_sensitive(home_start_, !starting_);

    bool mounted = false;
    if (status_ && status_->mountpoint) {
        mounted = mountinfo_has(read_file("/proc/self/mountinfo"), *status_->mountpoint);
        set_subtitle(home_mount_, *status_->mountpoint + (mounted ? "" : " (not mounted)"));
    } else if (running_) {
        set_subtitle(home_mount_, "Not mounted");
    } else {
        set_subtitle(home_mount_, mount_dir() + ", when GhostDrive is running");
    }
    gtk_widget_set_sensitive(open_button_, mounted);

    set_subtitle(home_rendezvous_,
                 status_ ? rendezvous_summary(*status_) : "Shown while GhostDrive is running");
    const bool discovery = status_ && status_->discovery_error;
    gtk_widget_set_visible(home_discovery_, discovery);
    if (discovery) set_subtitle(home_discovery_, *status_->discovery_error);
}

void AppWindow::update_devices() {
    std::optional<std::vector<Peer>> peers;
    if (status_) peers = status_->peers;
    if (shown_once_ && peers == shown_peers_) return;
    shown_peers_ = peers;
    for (GtkWidget* row : device_rows_) {
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(devices_group_), row);
    }
    device_rows_.clear();

    auto add = [&](GtkWidget* row) {
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(devices_group_), row);
        device_rows_.push_back(row);
    };
    if (!peers) {
        add(action_row("GhostDrive is not running",
                       "Start it on the Home page to see the other devices."));
        return;
    }
    if (peers->empty()) {
        GtkWidget* row = action_row("No other devices yet", "Invite one from Add device.");
        GtkWidget* go = button("Add device");
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), go);
        on(go, "clicked", [this] { adw_view_stack_set_visible_child_name(views_, "add"); });
        add(row);
        return;
    }
    for (const Peer& peer : *peers) {
        auto state = peer_state(peer);
        std::string subtitle = std::string(peer_state_label(state)) + "\nID " + peer.id;
        GtkWidget* row = action_row(peer.name.c_str(), subtitle);
        const char* icon = state == PeerState::offline   ? "network-offline-symbolic"
                           : state == PeerState::relayed ? "network-cellular-symbolic"
                                                         : "network-wired-symbolic";
        GtkWidget* image = gtk_image_new_from_icon_name(icon);
        gtk_widget_add_css_class(image, state == PeerState::offline   ? "dim-label"
                                        : state == PeerState::relayed ? "warning"
                                                                      : "success");
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), image);
        GtkWidget* remove = button("Remove", "flat");
        gtk_widget_add_css_class(remove, "error");
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
        on(remove, "clicked", [this, peer] { confirm_remove(peer); });
        add(row);
    }
}

void AppWindow::update_run_controls() {
    set_subtitle(run_row_, running_ ? "Running" : (starting_ ? "Starting..." : "Stopped"));
    gtk_button_set_label(GTK_BUTTON(run_button_), running_ ? "Stop" : "Start");
    gtk_widget_set_sensitive(run_button_, has_drive_ && !starting_);
    gtk_widget_set_sensitive(invite_button_, running_);
    const bool hint = !running_ && invite_code_.empty();
    if (hint) {
        gtk_widget_remove_css_class(invite_message_, "error");
        gtk_label_set_text(GTK_LABEL(invite_message_),
                           "Start GhostDrive to create an invite code.");
        gtk_widget_set_visible(invite_message_, TRUE);
    } else if (invite_hint_) {
        gtk_widget_set_visible(invite_message_, FALSE);
    }
    invite_hint_ = hint;
}

void AppWindow::refresh_settings() {
    if (!has_drive_) return;
    run_cli(commands::autostart_status(paths()), 15, [this](CliResult r) {
        if (!alive_) return;
        autostart_ = r.ok() ? parse_autostart_status(r.out) : std::nullopt;
        updating_ = true;
        adw_switch_row_set_active(ADW_SWITCH_ROW(autostart_row_),
                                  autostart_ && autostart_->configured);
        updating_ = false;
    });
    run_cli(commands::rendezvous_show(paths()), 15, [this](CliResult r) {
        if (!alive_) return;
        if (!r.ok()) {
            set_subtitle(rv_current_, plain_error(r));
            return;
        }
        auto setting = parse_rendezvous_setting(r.out);
        if (!setting) {
            set_subtitle(rv_current_, "Unknown");
        } else if (!setting->configured) {
            set_subtitle(rv_current_, "None. Devices find each other on the local network only.");
        } else {
            set_subtitle(rv_current_, setting->address + "\n" + setting->fingerprint);
        }
    });
}

// ---- First run ----

void AppWindow::set_setup_busy(bool busy, const std::string& message, bool join) {
    setup_busy_ = busy;
    GtkWidget* spinner = join ? join_spinner_ : create_spinner_;
    GtkWidget* text = join ? join_message_ : create_message_;
    if (busy) {
        gtk_spinner_start(GTK_SPINNER(spinner));
    } else {
        gtk_spinner_stop(GTK_SPINNER(spinner));
    }
    gtk_widget_remove_css_class(text, "error");
    gtk_label_set_text(GTK_LABEL(text), message.c_str());
    for (GtkWidget* w : {create_button_, create_back_, create_name_, join_button_, join_back_,
                         join_name_, join_code_}) {
        gtk_widget_set_sensitive(w, !busy);
    }
}

void AppWindow::setup_failed(const std::string& message, bool join) {
    set_setup_busy(false, message, join);
    gtk_widget_add_css_class(join ? join_message_ : create_message_, "error");
}

void AppWindow::setup_done(const std::string& text) {
    has_drive_ = drive_exists();
    show_root();
    adw_view_stack_set_visible_child_name(views_, "home");
    if (!text.empty()) toast(text);
    start_ghostdrive();
    refresh_settings();
}

void AppWindow::create_drive() {
    if (setup_busy_) return;
    std::string name = trim(entry_text(create_name_));
    if (auto problem = device_name_problem(name)) return setup_failed(*problem, false);
    set_setup_busy(true, "Creating the drive...", false);
    run_cli(commands::init(paths(), name), 30, [this](CliResult r) {
        if (!alive_) return;
        if (!r.ok()) return setup_failed(plain_error(r), false);
        set_setup_busy(false, "", false);
        setup_done("Drive created");
    });
}

void AppWindow::join_drive() {
    if (setup_busy_) return;
    auto code = extract_invite_code(entry_text(join_code_));
    if (!code) {
        return setup_failed("That does not look like an invite code. It starts with gd1-.", true);
    }
    std::string name = trim(entry_text(join_name_));
    if (auto problem = device_name_problem(name)) return setup_failed(*problem, true);
    set_setup_busy(true, "Contacting the inviting device...", true);
    run_cli(commands::join(paths(), *code, name), 90, [this](CliResult r) {
        if (!alive_) return;
        if (!r.ok()) return setup_failed(plain_error(r), true);
        gtk_editable_set_text(GTK_EDITABLE(join_code_), "");
        set_setup_busy(false, "", true);
        setup_done("Joined the drive");
    });
}

// ---- Actions ----

void AppWindow::start_ghostdrive() {
    if (!has_drive_ || running_ || starting_) return;
    std::string mount = mount_dir();
    g_mkdir_with_parents(mount.c_str(), 0700);
    std::string log = data_dir() + "/ghostdrived.log";
    auto error = start_daemon(commands::daemon(paths()), log, [this, log](int status) {
        if (!alive_) return;
        starting_ = false;
        if (status > 0) {
            std::string body = "ghostdrived exited with status " + std::to_string(status) + ".";
            if (auto tail = log_tail(log); !tail.empty()) body += "\n\n" + tail;
            body += "\n\nThe full log is " + clean_text(log, 200) + ".";
            alert("GhostDrive stopped", body);
        }
        poll();
    });
    if (error) return alert("Could not start GhostDrive", clean_text(*error, 300));
    starting_ = true;
    starting_since_ = g_get_monotonic_time();
    update_home();
    update_run_controls();
    // Poll soon instead of waiting for the next tick; ghostdrived answers within a second.
    g_timeout_add(
        800,
        [](gpointer self) -> gboolean {
            static_cast<AppWindow*>(self)->poll();
            return G_SOURCE_REMOVE;
        },
        this);
}

void AppWindow::stop_ghostdrive() {
    gtk_widget_set_sensitive(run_button_, FALSE);
    run_cli(commands::stop(paths()), 30, [this](CliResult r) {
        if (!alive_) return;
        gtk_widget_set_sensitive(run_button_, TRUE);
        if (r.ok()) {
            toast("GhostDrive stopped");
        } else {
            alert("Could not stop GhostDrive", plain_error(r));
        }
        poll();
    });
}

void AppWindow::confirm_remove(const Peer& peer) {
    std::string heading = "Remove " + peer.name + "?";
    AdwDialog* dialog = adw_alert_dialog_new(
        heading.c_str(),
        "It will no longer be able to connect to this drive's devices, and the other devices "
        "learn of it. This cannot be undone: to use that device again, it has to be reset and "
        "invited afresh.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "remove", "Remove",
                                   nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    on_response(dialog, [this, peer](const std::string& response) {
        if (response == "remove") remove_device(peer);
    });
    adw_dialog_present(dialog, GTK_WIDGET(window_));
}

void AppWindow::remove_device(const Peer& peer) {
    // By ID: names need not be unique.
    run_cli(commands::remove(paths(), peer.id), 30, [this, peer](CliResult r) {
        if (!alive_) return;
        if (r.ok()) {
            std::string text = plain_output(r);
            alert(peer.name + " was removed", text.empty() ? "Done." : text);
        } else {
            alert("Could not remove " + peer.name, plain_error(r));
        }
        poll();
    });
}

void AppWindow::create_invite() {
    gtk_widget_set_sensitive(invite_button_, FALSE);
    run_cli(commands::invite(paths(), kInviteSeconds), 15, [this](CliResult r) {
        if (!alive_) return;
        gtk_widget_set_sensitive(invite_button_, running_);
        auto invite = r.ok() ? parse_invite(r.out) : std::nullopt;
        if (!invite) {
            gtk_label_set_text(
                GTK_LABEL(invite_message_),
                r.ok() ? "The ghostdrive tool printed no invite code." : plain_error(r).c_str());
            gtk_widget_add_css_class(invite_message_, "error");
            gtk_widget_set_visible(invite_message_, TRUE);
            invite_hint_ = false;
            return;
        }
        invite_code_ = invite->code;
        long seconds = invite->minutes ? *invite->minutes * 60 : kInviteSeconds;
        invite_expires_ = g_get_monotonic_time() + static_cast<gint64>(seconds) * G_USEC_PER_SEC;
        gtk_label_set_text(GTK_LABEL(code_label_), invite_code_.c_str());
        gtk_widget_remove_css_class(code_label_, "dim-label");
        gtk_widget_set_visible(code_label_, TRUE);
        gtk_widget_set_visible(countdown_label_, TRUE);
        gtk_widget_set_visible(copy_button_, TRUE);
        gtk_widget_set_visible(invite_message_, FALSE);
        gtk_widget_remove_css_class(invite_message_, "error");
        gtk_button_set_label(GTK_BUTTON(invite_button_), "New code");
        tick_invite();
        if (!invite_timer_) {
            invite_timer_ = g_timeout_add_seconds(
                1,
                [](gpointer self) -> gboolean {
                    auto* w = static_cast<AppWindow*>(self);
                    w->tick_invite();
                    if (!w->invite_code_.empty()) return G_SOURCE_CONTINUE;
                    w->invite_timer_ = 0;
                    return G_SOURCE_REMOVE;
                },
                this);
        }
    });
}

void AppWindow::tick_invite() {
    if (invite_code_.empty()) return;
    long left = static_cast<long>((invite_expires_ - g_get_monotonic_time()) / G_USEC_PER_SEC);
    if (left <= 0) {
        invite_code_.clear();
        gtk_label_set_text(GTK_LABEL(countdown_label_), "This code has expired. Create a new one.");
        gtk_widget_add_css_class(code_label_, "dim-label");
        gtk_widget_set_visible(copy_button_, FALSE);
        gtk_button_set_label(GTK_BUTTON(invite_button_), "Create invite code");
        return;
    }
    std::string text = "Valid for " + format_countdown(left) + ". It works once.";
    gtk_label_set_text(GTK_LABEL(countdown_label_), text.c_str());
}

void AppWindow::set_autostart(bool on) {
    gtk_widget_set_sensitive(autostart_row_, FALSE);
    auto args = on ? commands::autostart_on(paths()) : commands::autostart_off(paths());
    run_cli(args, 60, [this, on](CliResult r) {
        if (!alive_) return;
        gtk_widget_set_sensitive(autostart_row_, TRUE);
        if (r.ok()) {
            toast(on ? "GhostDrive will start at login"
                     : "GhostDrive will no longer start at login");
        } else {
            alert(on ? "Could not turn on Start at login" : "Could not turn off Start at login",
                  plain_error(r));
        }
        refresh_settings();
        poll();
    });
}

void AppWindow::save_rendezvous() {
    std::string address = trim(entry_text(rv_address_));
    if (auto problem = rendezvous_address_problem(address))
        return alert("Check the address", *problem);
    auto fingerprint = normalize_fingerprint(entry_text(rv_fingerprint_));
    if (!fingerprint) {
        return alert("Check the fingerprint",
                     "Enter the 52-character fingerprint that ghostdrive-rendezvous printed when "
                     "it started.");
    }
    gtk_widget_set_sensitive(rv_save_, FALSE);
    run_cli(commands::rendezvous_set(paths(), address, *fingerprint), 15, [this](CliResult r) {
        if (!alive_) return;
        gtk_widget_set_sensitive(rv_save_, TRUE);
        if (r.ok()) {
            gtk_editable_set_text(GTK_EDITABLE(rv_address_), "");
            gtk_editable_set_text(GTK_EDITABLE(rv_fingerprint_), "");
            alert("Rendezvous server set", plain_output(r));
        } else {
            alert("Could not set the rendezvous server", plain_error(r));
        }
        refresh_settings();
    });
}

void AppWindow::apply_folder(bool data) {
    GtkWidget* entry = data ? data_entry_ : mount_entry_;
    std::string value = trim(entry_text(entry));
    if (auto problem = path_problem(value)) {
        return alert(data ? "Check the data folder" : "Check the mount location", *problem);
    }
    if (data) {
        settings_.data_dir = value;
        options_.data.reset();
    } else {
        settings_.mountpoint = value;
    }
    save_settings();
    if (data) {
        has_drive_ = drive_exists();
        status_.reset();
        running_ = false;
        id_fallback_.clear();
        show_root();
        poll();
        refresh_settings();
        toast("Data folder saved");
    } else {
        toast("Saved. It takes effect the next time GhostDrive starts.");
    }
}

void AppWindow::open_drive() {
    if (!status_ || !status_->mountpoint) return;
    GFile* file = g_file_new_for_path(status_->mountpoint->c_str());
    GtkFileLauncher* launcher = gtk_file_launcher_new(file);
    g_object_unref(file);
    gtk_file_launcher_launch(
        launcher, window_, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer) {
            gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, nullptr);
            g_object_unref(source);
        },
        nullptr);
}

void AppWindow::toast(const std::string& text) {
    AdwToast* t = adw_toast_new("");
    adw_toast_set_use_markup(t, FALSE);
    adw_toast_set_title(t, text.c_str());
    adw_toast_overlay_add_toast(toasts_, t);
}

void AppWindow::alert(const std::string& heading, const std::string& body) {
    AdwDialog* dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Close");
    adw_dialog_present(dialog, GTK_WIDGET(window_));
}

}  // namespace ghostdrive::app
