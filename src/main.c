/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <glob.h>
#include <signal.h>
#include <string.h>

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

typedef struct {
    GtkWidget *window;
    GtkDropDown *device_dropdown;
    GtkDropDown *baud_dropdown;
    GtkCheckButton *timestamp_check;
    GtkCheckButton *log_check;
    GtkEntry *log_directory_entry;
    GtkButton *connect_button;
    GtkLabel *status_label;
    VteTerminal *terminal;
    GPid child_pid;
    gchar **spawn_argv;
} TioGui;

static const char *const device_patterns[] = {
    "/dev/serial/by-id/*",
    "/dev/ttyUSB*",
    "/dev/ttyACM*",
    NULL,
};

static const char *const baud_rates[] = {
    "9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600", NULL,
};

static void set_status(TioGui *gui, const char *message)
{
    gtk_label_set_text(gui->status_label, message);
}

static gint compare_strings(gconstpointer left, gconstpointer right)
{
    const char *const *left_string = left;
    const char *const *right_string = right;
    return g_strcmp0(*left_string, *right_string);
}

static void refresh_devices(TioGui *gui)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *devices = g_ptr_array_new_with_free_func(g_free);

    for (size_t pattern_index = 0; device_patterns[pattern_index] != NULL; ++pattern_index) {
        glob_t matches = {0};
        if (glob(device_patterns[pattern_index], GLOB_NOSORT, NULL, &matches) == 0) {
            for (size_t match_index = 0; match_index < matches.gl_pathc; ++match_index) {
                const char *path = matches.gl_pathv[match_index];
                if (!g_hash_table_contains(seen, path)) {
                    g_hash_table_add(seen, g_strdup(path));
                    g_ptr_array_add(devices, g_strdup(path));
                }
            }
        }
        globfree(&matches);
    }

    g_ptr_array_sort(devices, compare_strings);

    GtkStringList *model = gtk_string_list_new(NULL);
    for (guint index = 0; index < devices->len; ++index) {
        gtk_string_list_append(model, g_ptr_array_index(devices, index));
    }
    gtk_drop_down_set_model(gui->device_dropdown, G_LIST_MODEL(model));
    g_object_unref(model);

    if (devices->len > 0) {
        gtk_drop_down_set_selected(gui->device_dropdown, 0);
        g_autofree gchar *message = g_strdup_printf("Found %u serial device%s",
                                                     devices->len,
                                                     devices->len == 1 ? "" : "s");
        set_status(gui, message);
    } else {
        set_status(gui, "No serial devices found");
    }

    g_ptr_array_unref(devices);
    g_hash_table_unref(seen);
}

static const char *selected_string(GtkDropDown *dropdown)
{
    GtkStringObject *item = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(dropdown));
    return item == NULL ? NULL : gtk_string_object_get_string(item);
}

static void on_child_exited(VteTerminal *terminal, gint status, gpointer user_data)
{
    (void)terminal;
    TioGui *gui = user_data;

    gui->child_pid = -1;
    gtk_widget_set_sensitive(GTK_WIDGET(gui->connect_button), TRUE);
    gtk_button_set_label(gui->connect_button, "Connect");

    g_autofree gchar *message = g_strdup_printf("Disconnected (tio exit status: %d)", status);
    set_status(gui, message);
}

static void on_spawn_finished(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
    (void)terminal;
    TioGui *gui = user_data;

    g_clear_pointer(&gui->spawn_argv, g_strfreev);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->connect_button), TRUE);

    if (error != NULL) {
        g_autofree gchar *message = g_strdup_printf("Could not start tio: %s", error->message);
        set_status(gui, message);
        gui->child_pid = -1;
        gtk_button_set_label(gui->connect_button, "Connect");
        return;
    }

    gui->child_pid = pid;
    gtk_button_set_label(gui->connect_button, "Disconnect");
    set_status(gui, "Connected");
    gtk_widget_grab_focus(GTK_WIDGET(gui->terminal));
}

static gchar **build_tio_argv(TioGui *gui, const char *device, const char *baud)
{
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(arguments, g_strdup("tio"));
    g_ptr_array_add(arguments, g_strdup("--baudrate"));
    g_ptr_array_add(arguments, g_strdup(baud));
    g_ptr_array_add(arguments, g_strdup("--databits"));
    g_ptr_array_add(arguments, g_strdup("8"));
    g_ptr_array_add(arguments, g_strdup("--flow"));
    g_ptr_array_add(arguments, g_strdup("none"));
    g_ptr_array_add(arguments, g_strdup("--stopbits"));
    g_ptr_array_add(arguments, g_strdup("1"));
    g_ptr_array_add(arguments, g_strdup("--parity"));
    g_ptr_array_add(arguments, g_strdup("none"));

    if (gtk_check_button_get_active(gui->timestamp_check)) {
        g_ptr_array_add(arguments, g_strdup("--timestamp"));
    }
    if (gtk_check_button_get_active(gui->log_check)) {
        g_ptr_array_add(arguments, g_strdup("--log"));
        g_ptr_array_add(arguments, g_strdup("--log-directory"));
        g_ptr_array_add(arguments,
                        g_strdup(gtk_editable_get_text(GTK_EDITABLE(gui->log_directory_entry))));
    }

    g_ptr_array_add(arguments, g_strdup(device));
    g_ptr_array_add(arguments, NULL);
    return (gchar **)g_ptr_array_free(arguments, FALSE);
}

static void disconnect_tio(TioGui *gui)
{
    if (gui->child_pid <= 0) {
        return;
    }

    if (kill(gui->child_pid, SIGHUP) == -1 && errno != ESRCH) {
        g_autofree gchar *message = g_strdup_printf("Could not stop tio: %s", g_strerror(errno));
        set_status(gui, message);
        return;
    }

    set_status(gui, "Disconnecting…");
}

static void connect_tio(TioGui *gui)
{
    const char *device = selected_string(gui->device_dropdown);
    const char *baud = selected_string(gui->baud_dropdown);

    if (device == NULL || baud == NULL) {
        set_status(gui, "Select a serial device and baud rate first");
        return;
    }

    g_autofree gchar *tio_path = g_find_program_in_path("tio");
    if (tio_path == NULL) {
        set_status(gui, "tio was not found in PATH");
        return;
    }

    if (gtk_check_button_get_active(gui->log_check)) {
        const char *log_directory =
            gtk_editable_get_text(GTK_EDITABLE(gui->log_directory_entry));
        if (log_directory[0] == '\0') {
            set_status(gui, "Choose a log directory first");
            return;
        }
        if (g_mkdir_with_parents(log_directory, 0750) == -1) {
            g_autofree gchar *message =
                g_strdup_printf("Could not create log directory: %s", g_strerror(errno));
            set_status(gui, message);
            return;
        }
    }

    vte_terminal_reset(gui->terminal, TRUE, TRUE);
    gui->spawn_argv = build_tio_argv(gui, device, baud);
    set_status(gui, "Connecting…");
    gtk_widget_set_sensitive(GTK_WIDGET(gui->connect_button), FALSE);

    vte_terminal_spawn_async(gui->terminal,
                             VTE_PTY_DEFAULT,
                             NULL,
                             gui->spawn_argv,
                             NULL,
                             G_SPAWN_SEARCH_PATH,
                             NULL,
                             NULL,
                             NULL,
                             -1,
                             NULL,
                             on_spawn_finished,
                             gui);

}

static void on_connect_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;

    if (gui->child_pid > 0) {
        disconnect_tio(gui);
    } else {
        connect_tio(gui);
    }
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    refresh_devices(user_data);
}

static void on_log_toggled(GtkCheckButton *button, gpointer user_data)
{
    TioGui *gui = user_data;
    gtk_widget_set_sensitive(GTK_WIDGET(gui->log_directory_entry),
                             gtk_check_button_get_active(button));
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    TioGui *gui = user_data;

    if (gui->child_pid > 0) {
        (void)kill(gui->child_pid, SIGHUP);
    }
    g_clear_pointer(&gui->spawn_argv, g_strfreev);
}

static GtkWidget *make_label(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

static void activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;

    TioGui *gui = g_new0(TioGui, 1);
    gui->child_pid = -1;

    gui->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(gui->window), "tio-gui");
    gtk_window_set_default_size(GTK_WINDOW(gui->window), 1000, 680);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(gui->window), root);

    GtkWidget *controls = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(controls), 8);
    gtk_grid_set_row_spacing(GTK_GRID(controls), 8);
    gtk_box_append(GTK_BOX(root), controls);

    gtk_grid_attach(GTK_GRID(controls), make_label("Device"), 0, 0, 1, 1);
    gui->device_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->device_dropdown), TRUE);
    gtk_grid_attach(GTK_GRID(controls), GTK_WIDGET(gui->device_dropdown), 1, 0, 1, 1);

    GtkWidget *refresh_button = gtk_button_new_with_label("Refresh");
    gtk_grid_attach(GTK_GRID(controls), refresh_button, 2, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(controls), make_label("Baud"), 3, 0, 1, 1);
    GtkStringList *baud_model = gtk_string_list_new(baud_rates);
    gui->baud_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(baud_model), NULL));
    g_object_unref(baud_model);
    gtk_drop_down_set_selected(gui->baud_dropdown, 4);
    gtk_grid_attach(GTK_GRID(controls), GTK_WIDGET(gui->baud_dropdown), 4, 0, 1, 1);

    gui->timestamp_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Timestamps"));
    gtk_grid_attach(GTK_GRID(controls), GTK_WIDGET(gui->timestamp_check), 0, 1, 1, 1);

    gui->log_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Log session"));
    gtk_grid_attach(GTK_GRID(controls), GTK_WIDGET(gui->log_check), 1, 1, 1, 1);

    gui->connect_button = GTK_BUTTON(gtk_button_new_with_label("Connect"));
    gtk_widget_add_css_class(GTK_WIDGET(gui->connect_button), "suggested-action");
    gtk_grid_attach(GTK_GRID(controls), GTK_WIDGET(gui->connect_button), 4, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(controls), make_label("Log directory"), 0, 2, 1, 1);
    gui->log_directory_entry = GTK_ENTRY(gtk_entry_new());
    g_autofree gchar *default_log_directory =
        g_build_filename(g_get_user_state_dir(), "tio-gui", NULL);
    gtk_editable_set_text(GTK_EDITABLE(gui->log_directory_entry), default_log_directory);
    gtk_widget_set_hexpand(GTK_WIDGET(gui->log_directory_entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->log_directory_entry), FALSE);
    gtk_grid_attach(GTK_GRID(controls), GTK_WIDGET(gui->log_directory_entry), 1, 2, 4, 1);

    gui->status_label = GTK_LABEL(gtk_label_new("Ready"));
    gtk_label_set_xalign(gui->status_label, 0.0F);
    gtk_widget_add_css_class(GTK_WIDGET(gui->status_label), "dim-label");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->status_label));

    gui->terminal = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scrollback_lines(gui->terminal, 10000);
    vte_terminal_set_mouse_autohide(gui->terminal, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(gui->terminal), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(gui->terminal), TRUE);

    GtkWidget *terminal_frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(terminal_frame), GTK_WIDGET(gui->terminal));
    gtk_widget_set_hexpand(terminal_frame, TRUE);
    gtk_widget_set_vexpand(terminal_frame, TRUE);
    gtk_box_append(GTK_BOX(root), terminal_frame);

    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), gui);
    g_signal_connect(gui->connect_button, "clicked", G_CALLBACK(on_connect_clicked), gui);
    g_signal_connect(gui->log_check, "toggled", G_CALLBACK(on_log_toggled), gui);
    g_signal_connect(gui->terminal, "child-exited", G_CALLBACK(on_child_exited), gui);
    g_signal_connect(gui->window, "destroy", G_CALLBACK(on_window_destroy), gui);

    refresh_devices(gui);
    gtk_window_present(GTK_WINDOW(gui->window));
}

int main(int argc, char **argv)
{
    g_autoptr(GtkApplication) application =
        gtk_application_new("io.github.keithxc.tio_gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    return g_application_run(G_APPLICATION(application), argc, argv);
}
