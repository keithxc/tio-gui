/* SPDX-License-Identifier: GPL-3.0-only */
/* Native transport adapters for the same serial tools used by the Linux UI. */
static gboolean sequence_connected(Tab *tab) { return tab->connected && !tab->bridge; }
static gboolean sequence_save(Tab *tab, GError **error) { return save(tab->app, error); }
static TioSequenceSendResult sequence_send(const GByteArray *payload, gpointer data)
{
    Tab *tab = data;
    if (!tab->connected || tab->bridge) return TIO_SEQUENCE_ERROR;
    if (!tio_native_idle(tab->serial)) return TIO_SEQUENCE_WAIT;
    if (!payload->len) return TIO_SEQUENCE_ACCEPT;
    g_autoptr(GBytes) bytes = g_bytes_new(payload->data, payload->len);
    return tio_native_send(tab->serial, bytes, NULL) ? TIO_SEQUENCE_ACCEPT : TIO_SEQUENCE_ERROR;
}
#include "sequence_ui.h"
static void cancel_sends(Tab *tab)
{
    g_clear_handle_id(&tab->quick_send_timer, g_source_remove);
    g_clear_pointer(&tab->quick_pending, g_bytes_unref);
    g_clear_pointer(&tab->sequence_runner, tio_sequence_runner_free);
}
static gboolean quick_due(gpointer data)
{
    Tab *tab = data; tab->quick_send_timer = 0;
    g_autoptr(GError) error = NULL;
    if (!tio_native_send(tab->serial, tab->quick_pending, &error)) set_status(tab, error->message);
    g_clear_pointer(&tab->quick_pending, g_bytes_unref);
    return G_SOURCE_REMOVE;
}
static void quick_clicked(GtkButton *b, gpointer data)
{
    Tab *tab = data; guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "index"));
    if (!tab->connected || tab->bridge || tab->quick_send_timer || tio_sequence_runner_active(tab->sequence_runner)) {
        set_status(tab, _("Stop the current send before starting another")); return;
    }
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) payload = tio_payload_build(tab->config.quick_payloads[i], tab->config.quick_modes[i],
        tab->config.quick_endings[i], tab->config.quick_crcs[i], &error);
    if (!payload) { set_status(tab, error->message); return; }
    tab->quick_pending = g_bytes_new(payload->data, payload->len);
    if (tab->config.quick_delays[i]) tab->quick_send_timer = g_timeout_add(tab->config.quick_delays[i], quick_due, tab);
    else quick_due(tab);
}
static GtkSpinButton *tool_spin(GtkWidget *box, const char *label, guint maximum, guint value)
{
    append(box, gtk_label_new(label));
    GtkSpinButton *spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, maximum, 1));
    gtk_spin_button_set_value(spin, value); append(box, GTK_WIDGET(spin)); return spin;
}
static GtkWidget *tool_section(Tab *tab, const char *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_widget_add_css_class(box, "settings-card");
    gtk_stack_add_titled(tab->settings_stack, box, NULL, title); return box;
}
static void tools_snapshot(Tab *tab)
{
    if (!tab->timestamp_format) return;
    const char *formats[] = {"24hour", "24hour-start", "24hour-delta", "iso8601", "epoch"};
    tab->config.auto_connect = choice(tab->auto_connect); replace(&tab->config.exclude_devices, entry(tab->exclude_devices));
    tab->config.connection_notify = checked(tab->connection_notify); tab->config.connection_sound = checked(tab->connection_sound);
    tab->config.timestamps = checked(tab->timestamps);
    replace(&tab->config.timestamp_format, formats[MIN(choice(tab->timestamp_format), 4)]);
    tab->config.log_append = checked(tab->log_append); tab->config.log_strip = checked(tab->log_strip);
    tab->config.dtr_default = choice(tab->dtr_default); tab->config.rts_default = choice(tab->rts_default);
    tab->config.line_pulse_ms = gtk_spin_button_get_value_as_int(tab->pulse_ms);
    tab->config.output_delay = gtk_spin_button_get_value_as_int(tab->byte_delay);
    tab->config.output_line_delay = gtk_spin_button_get_value_as_int(tab->line_delay);
    tab->config.capture_part_mb = gtk_spin_button_get_value_as_int(tab->capture_part_spin);
    tab->config.capture_part_seconds = gtk_spin_button_get_value_as_int(tab->capture_time_spin);
    tab->config.capture_keep_files = gtk_spin_button_get_value_as_int(tab->capture_keep_spin);
    tab->config.capture_disk_mb = gtk_spin_button_get_value_as_int(tab->capture_disk_spin);
}
static void pulse_clicked(GtkButton *button_, gpointer data)
{
    Tab *tab = data; guint line = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button_), "pulse-line"));
    g_autoptr(GError) error = NULL;
    if (!tio_native_pulse(tab->serial, line, gtk_spin_button_get_value_as_int(tab->pulse_ms), &error)) set_status(tab, error->message);
}
static gboolean capture_ui_tick(gpointer data)
{
    Tab *tab = data; const char *error = tio_capture_error(tab->capture);
    const char *path = tio_capture_path(tab->capture);
    g_autofree gchar *message = error ? g_strdup_printf(_("Recording failed: %s"), error)
        : g_strdup_printf(tio_capture_finished(tab->capture) ? _("Recording saved: %s") : _("Recording: %s"), path ? path : "");
    gtk_label_set_text(tab->capture_label, message);
    if (tio_capture_finished(tab->capture)) {
        gtk_button_set_label(tab->capture_button, _("Start recording…")); tab->capture_timer = 0; return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
static gboolean start_capture(Tab *tab, const char *path, GError **error)
{
    if (!tab->connected || (tab->capture && !tio_capture_finished(tab->capture))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Connect first and finish any current recording"); return FALSE;
    }
    snapshot(tab);
    g_autofree gchar *metadata = g_strdup_printf("device=%s baud=%s bits=%s stops=%s parity=%s flow=%s",
        tab->config.device, tab->config.baud, tab->config.data_bits, tab->config.stop_bits, tab->config.parity, tab->config.flow);
    TioCapture *capture = tio_capture_new(path, (guint64)tab->config.capture_part_mb * 1024 * 1024,
        tab->config.capture_part_seconds, tab->config.capture_keep_files,
        (guint64)tab->config.capture_disk_mb * 1024 * 1024, metadata, error);
    if (!capture) return FALSE;
    g_clear_pointer(&tab->capture, tio_capture_unref); tab->capture = capture;
    tio_capture_record(capture, TIO_CAPTURE_PARAMETERS, (const guint8 *)metadata, strlen(metadata), g_get_real_time());
    tio_capture_record(capture, TIO_CAPTURE_CONNECT, (const guint8 *)entry(tab->device), strlen(entry(tab->device)), g_get_real_time());
    gtk_button_set_label(tab->capture_button, _("Stop recording"));
    g_clear_handle_id(&tab->capture_timer, g_source_remove); tab->capture_timer = g_timeout_add(200, capture_ui_tick, tab);
    return TRUE;
}
static void capture_selected(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GtkWidget) page = data; Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!tab || !file) return;
    g_autofree gchar *path = g_file_get_path(file); g_autoptr(GError) error = NULL;
    if (!start_capture(tab, path, &error)) set_status(tab, error->message);
}
static void on_capture_clicked(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data;
    if (tab->capture && !tio_capture_finished(tab->capture)) { tio_capture_stop(tab->capture); return; }
    if (!tab->connected) { set_status(tab, _("Connect before starting a recording")); return; }
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_initial_name(dialog, "serial.tiocap");
    gtk_file_dialog_save(dialog, tab->app->window, NULL, capture_selected, g_object_ref(tab->page));
}
static void transfer_update(Tab *tab)
{
    if (tab->modbus_window && tab->bridge) {
        g_autoptr(GError) error = NULL;
        if (!tab->connected || !tio_native_bridge_tick(tab->bridge, &error)) {
            gtk_window_destroy(GTK_WINDOW(tab->modbus_window));
            g_clear_pointer(&tab->bridge, tio_native_bridge_free);
        }
        return;
    }
    if (!tab->transfer) { g_clear_pointer(&tab->bridge, tio_native_bridge_free); return; }
    if (!tab->connected && tio_transfer_active(tab->transfer)) tio_transfer_cancel(tab->transfer);
    if (tab->bridge) {
        g_autoptr(GError) error = NULL;
        if (!tio_native_bridge_tick(tab->bridge, &error)) tio_transfer_cancel(tab->transfer);
    }
    gtk_label_set_text(tab->transfer_status, tio_transfer_status(tab->transfer));
    if (!tio_transfer_active(tab->transfer) && tab->bridge) {
        if (!tab->transfer_drain_deadline) tab->transfer_drain_deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
        if (tio_native_bridge_drained(tab->bridge) || !tab->connected || g_get_monotonic_time() > tab->transfer_drain_deadline)
            g_clear_pointer(&tab->bridge, tio_native_bridge_free);
    }
}
static gboolean start_transfer(Tab *tab, const char *path, gboolean receive, GError **error)
{
    if (!tab->connected || !tio_native_idle(tab->serial) || tio_transfer_active(tab->transfer) ||
        tio_sequence_runner_active(tab->sequence_runner) || tab->quick_send_timer) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Connect and stop pending sends before transferring files"); return FALSE;
    }
    g_clear_pointer(&tab->transfer, tio_transfer_free); g_clear_pointer(&tab->bridge, tio_native_bridge_free);
    tab->transfer_drain_deadline = 0;
    tab->bridge = tio_native_bridge_new(tab->serial, error); if (!tab->bridge) return FALSE;
    tab->transfer = tio_transfer_start(tio_native_bridge_path(tab->bridge), path, choice(tab->transfer_protocol), receive,
        gtk_spin_button_get_value_as_int(tab->transfer_timeout), error);
    if (!tab->transfer) { g_clear_pointer(&tab->bridge, tio_native_bridge_free); return FALSE; }
    return TRUE;
}
static void transfer_selected(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GtkWidget) page = data; Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    gboolean receive = GPOINTER_TO_INT(g_object_get_data(source, "receive"));
    g_autoptr(GFile) file = receive ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!tab || !file) return;
    g_autofree gchar *path = g_file_get_path(file); g_autoptr(GError) error = NULL;
    if (!start_transfer(tab, path, receive, &error)) gtk_label_set_text(tab->transfer_status, error->message);
}
static void transfer_choose(GtkButton *b, gpointer data)
{
    Tab *tab = data; gboolean receive = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "receive"));
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new(); g_object_set_data(G_OBJECT(dialog), "receive", GINT_TO_POINTER(receive));
    if (receive) gtk_file_dialog_select_folder(dialog, tab->app->window, NULL, transfer_selected, g_object_ref(tab->page));
    else gtk_file_dialog_open(dialog, tab->app->window, NULL, transfer_selected, g_object_ref(tab->page));
}
static void transfer_cancel(GtkButton *b, gpointer data) { (void)b; Tab *tab = data; tio_transfer_cancel(tab->transfer); }
static void modbus_rtu(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data;
    if (!tab->connected || tab->bridge || !tio_native_idle(tab->serial) || tab->quick_send_timer || tio_sequence_runner_active(tab->sequence_runner)) {
        set_status(tab, _("Connect and stop pending sends before using Modbus RTU")); return;
    }
    g_autoptr(GError) error = NULL;
    tab->bridge = tio_native_bridge_new(tab->serial, &error);
    if (!tab->bridge) { set_status(tab, error->message); return; }
    tio_native_bridge_reusable(tab->bridge);
    tab->modbus_window = tio_modbus_window(tab->app->window, tio_native_bridge_path(tab->bridge));
    g_object_add_weak_pointer(G_OBJECT(tab->modbus_window), (gpointer *)&tab->modbus_window);
}
static void terminal_output(const char *data, gsize length, gpointer user)
{
    Tab *tab = user; g_autoptr(GBytes) bytes = g_bytes_new(data, length); send_bytes(tab, bytes);
}
static void highlight_changed(GtkCheckButton *b, gpointer data)
{
    Tab *tab = data; if (!tab->console_stack) return;
    gtk_stack_set_visible_child_name(tab->console_stack, checked(b) ? "highlight" : "terminal");
    gtk_widget_grab_focus(checked(b) ? GTK_WIDGET(tab->view) : GTK_WIDGET(tio_terminal_view(tab->terminal)));
}
static void selection_changed(GObject *buffer, GParamSpec *spec, gpointer data)
{
    (void)spec; Tab *tab = data; if (gtk_text_buffer_get_has_selection(GTK_TEXT_BUFFER(buffer))) gtk_check_button_set_active(tab->follow, FALSE);
}
static void tabs_reordered(GtkNotebook *notebook, GtkWidget *child, guint page, gpointer data)
{
    (void)child; (void)page; App *app = data; g_ptr_array_set_size(app->tabs, 0);
    for (gint i = 0; i < gtk_notebook_get_n_pages(notebook); i++) {
        Tab *tab = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(notebook, i)), "tab"); if (tab) g_ptr_array_add(app->tabs, tab);
    }
}
static void setup_terminal_input(Tab *tab, GtkTextView *view)
{
    g_signal_connect(gtk_text_view_get_buffer(view), "notify::has-selection", G_CALLBACK(selection_changed), tab);
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    GtkIMContext *im = gtk_im_multicontext_new(); gtk_im_context_set_client_widget(im, GTK_WIDGET(view));
    gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(keys), im);
    g_signal_connect(im, "commit", G_CALLBACK(committed), tab);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_object_set_data_full(G_OBJECT(focus), "im-context", g_object_ref(im), g_object_unref);
    g_signal_connect(focus, "enter", G_CALLBACK(input_focus_enter), im); g_signal_connect(focus, "leave", G_CALLBACK(input_focus_leave), im);
    gtk_widget_add_controller(GTK_WIDGET(view), focus); g_object_unref(im);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), tab); gtk_widget_add_controller(GTK_WIDGET(view), keys);
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE); g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_console), tab);
    gtk_widget_add_controller(GTK_WIDGET(view), scroll);
}
static void quick_cancel(GtkButton *b, gpointer data) { (void)b; Tab *tab = data; gtk_widget_set_visible(GTK_WIDGET(tab->quick_window), FALSE); }
typedef struct { GtkWidget *page; GtkWindow *window; GtkEntry *name; } RenameTab;
static void rename_free(gpointer data) { RenameTab *rename = data; g_object_unref(rename->page); g_free(rename); }
static void rename_apply(GtkButton *b, gpointer data)
{
    (void)b; RenameTab *rename = data; Tab *tab = g_object_get_data(G_OBJECT(rename->page), "tab");
    if (tab && !tab->closing) {
        replace(&tab->config.tab_name, entry(rename->name));
        gtk_label_set_text(tab->title, *tab->config.tab_name ? tab->config.tab_name : entry(tab->device));
        g_autoptr(GError) error = NULL; if (!save(tab->app, &error)) { set_status(tab, error->message); return; }
    }
    gtk_window_destroy(rename->window);
}
static void rename_tab(GtkGestureClick *gesture, int presses, double x, double y, gpointer data)
{
    (void)x; (void)y; if (presses != 2 && gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != 3) return; Tab *tab = data;
    RenameTab *rename = g_new0(RenameTab, 1); rename->page = g_object_ref(tab->page); rename->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(rename->window, _("Rename session")); gtk_window_set_transient_for(rename->window, tab->app->window);
    gtk_window_set_destroy_with_parent(rename->window, TRUE);
    g_object_set_data_full(G_OBJECT(rename->window), "rename", rename, rename_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); tio_ui_margins(box, 12); gtk_window_set_child(rename->window, box);
    rename->name = field(box, _("Name"), tab->config.tab_name, 24); gtk_entry_set_max_length(rename->name, 128);
    button(box, _("Save"), G_CALLBACK(rename_apply), rename); gtk_window_present(rename->window);
}
static GtkWidget *tool_hint(GtkWidget *box, const char *message)
{
    GtkWidget *hint = gtk_label_new(message); gtk_label_set_wrap(GTK_LABEL(hint), TRUE); gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_add_css_class(hint, "settings-hint"); append(box, hint); return hint;
}
static void tools_controls(Tab *tab)
{
    /* Same order and grouping as the Linux session panel. */
    GtkWidget *box = tool_section(tab, _("Session")), *r = row(); append(box, r);
    const char *formats[] = {_("Time"), _("Time since start"), _("Time since previous line"), "ISO 8601", "Epoch", NULL};
    const char *ids[] = {"24hour", "24hour-start", "24hour-delta", "iso8601", "epoch"}; guint selected = 0;
    for (guint i = 0; i < 5; i++) if (!g_strcmp0(ids[i], tab->config.timestamp_format)) selected = i;
    tab->timestamp_format = dropdown(r, _("Timestamp format"), formats, selected);
    r = row(); append(box, r);
    tab->byte_delay = tool_spin(r, _("Character delay (ms)"), 60000, tab->config.output_delay);
    tab->line_delay = tool_spin(r, _("Line delay (ms)"), 60000, tab->config.output_line_delay);
    r = row(); append(box, r);
    tab->log_append = check(r, _("Append log"), tab->config.log_append);
    tab->log_strip = check(r, _("Strip ANSI escapes in log"), tab->config.log_strip);
    box = tool_section(tab, _("Serial lines")); r = row(); append(box, r);
    const char *defaults[] = {_("Unchanged"), _("Low"), _("High"), NULL};
    tab->dtr_default = dropdown(r, _("DTR on connect"), defaults, tab->config.dtr_default);
    tab->rts_default = dropdown(r, _("RTS on connect"), defaults, tab->config.rts_default);
    tab->pulse_ms = tool_spin(r, _("Pulse (ms)"), 10000, tab->config.line_pulse_ms);
    gtk_spin_button_set_range(tab->pulse_ms, 1, 10000);
    r = row(); append(box, r);
    /* line_clicked decodes action / 2 as the line (DTR, RTS, Break) and action % 2 as the level. */
    const char *names[] = {_("Send Break"), _("DTR Low"), _("DTR High"), NULL, _("RTS Low"), _("RTS High"), NULL};
    const gint actions[] = {4, 0, 1, -1, 2, 3, -2};
    for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
        GtkWidget *b;
        if (actions[i] >= 0) {
            b = button(r, names[i], G_CALLBACK(line_clicked), tab); g_object_set_data(G_OBJECT(b), "line", GINT_TO_POINTER(actions[i]));
        } else {
            b = button(r, actions[i] == -1 ? _("DTR Pulse") : _("RTS Pulse"), G_CALLBACK(pulse_clicked), tab);
            g_object_set_data(G_OBJECT(b), "pulse-line", GUINT_TO_POINTER(actions[i] == -2));
        }
    }
    tool_hint(box, _("Defaults and output delays apply on the next connection. RS-485 automatic direction requires an adapter with hardware auto-direction on macOS."));
    box = tool_section(tab, _("Reconnect")); r = row(); append(box, r);
    append(r, GTK_WIDGET(tab->reconnect));
    const char *strategies[] = {_("Same device"), _("Next new device"), _("Latest device"), NULL};
    tab->auto_connect = dropdown(r, NULL, strategies, MIN(tab->config.auto_connect, 2));
    r = row(); append(box, r); tab->exclude_devices = field(r, _("Exclude devices (regex)"), tab->config.exclude_devices, 24);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->exclude_devices), TRUE);
    r = row(); append(box, r); tab->connection_notify = check(r, _("Desktop notifications"), tab->config.connection_notify);
    tab->connection_sound = check(r, _("Connection sound"), tab->config.connection_sound);
    tool_hint(box, _("USB adapters with a unique serial number reconnect by identity. Others retry the same path. New/latest selects a device when Connect is pressed."));
    button(tab->settings_actions, _("Modbus RTU…"), G_CALLBACK(modbus_rtu), tab);
    box = tool_section(tab, _("File transfer")); r = row(); append(box, r);
    const char *protocols[] = {"XMODEM-CRC", "YMODEM", "ZMODEM", NULL};
    tab->transfer_protocol = dropdown(r, _("Protocol"), protocols, 0);
    tab->transfer_timeout = tool_spin(r, _("Timeout (s)"), 86400, 120);
    gtk_spin_button_set_range(tab->transfer_timeout, 1, 86400);
    r = row(); append(box, r); button(r, _("Send file…"), G_CALLBACK(transfer_choose), tab);
    GtkWidget *b = button(r, _("Receive into folder…"), G_CALLBACK(transfer_choose), tab); g_object_set_data(G_OBJECT(b), "receive", GINT_TO_POINTER(1));
    button(r, _("Cancel transfer"), G_CALLBACK(transfer_cancel), tab);
    tab->transfer_status = GTK_LABEL(tool_hint(box, _("Choose an empty receive directory to protect existing files")));
    box = tool_section(tab, _("Recording")); r = row(); append(box, r);
    tab->capture_part_spin = tool_spin(r, _("Part MiB"), 128, MAX(1, tab->config.capture_part_mb));
    gtk_spin_button_set_range(tab->capture_part_spin, 1, 128);
    tab->capture_time_spin = tool_spin(r, _("Part seconds (0=off)"), 86400, tab->config.capture_part_seconds);
    r = row(); append(box, r);
    tab->capture_keep_spin = tool_spin(r, _("Keep parts"), 1000, MAX(1, tab->config.capture_keep_files));
    gtk_spin_button_set_range(tab->capture_keep_spin, 1, 1000);
    tab->capture_disk_spin = tool_spin(r, _("Disk MiB"), 1048576, MAX(1, tab->config.capture_disk_mb));
    gtk_spin_button_set_range(tab->capture_disk_spin, 1, 1048576);
    r = row(); append(box, r);
    tab->capture_button = GTK_BUTTON(button(r, _("Start recording…"), G_CALLBACK(on_capture_clicked), tab));
    tab->capture_label = GTK_LABEL(tool_hint(box, ""));
}
static void history_use(GtkButton *b, gpointer data) { Tab *tab = data; text(tab->send, gtk_button_get_label(b)); }
static void history_visible(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)spec; Tab *tab = data; if (!gtk_widget_get_visible(GTK_WIDGET(object))) return;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GPtrArray *history = tab->app->settings.history;
    for (guint i = history->len; i > 0; i--) button(box, g_ptr_array_index(history, i - 1), G_CALLBACK(history_use), tab);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box); gtk_popover_set_child(GTK_POPOVER(object), scroll);
}
static void setup_history(Tab *tab, GtkMenuButton *menu)
{
    GtkWidget *popover = gtk_popover_new(); gtk_menu_button_set_popover(menu, popover);
    g_signal_connect(popover, "notify::visible", G_CALLBACK(history_visible), tab);
}
static gboolean quick_validate(Tab *tab)
{
    if (!tab->quick_save_button) return TRUE;
    gboolean valid = TRUE;
    for (guint i = 0; i < 4; i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(GByteArray) payload = tio_payload_build(entry(tab->quick_text[i]), choice(tab->quick_mode[i]),
            choice(tab->quick_ending[i]), choice(tab->quick_crc[i]), &error);
        GString *preview = g_string_new("");
        if (!payload) { valid = FALSE; g_string_append(preview, error->message); }
        else {
            g_string_append_printf(preview, "%u bytes: ", payload->len);
            for (guint k = 0; k < MIN(payload->len, 128); k++) g_string_append_printf(preview, "%02X ", payload->data[k]);
        }
        gtk_label_set_text(tab->quick_preview[i], preview->str); g_string_free(preview, TRUE);
    }
    gtk_widget_set_sensitive(GTK_WIDGET(tab->quick_save_button), valid); return valid;
}
static void quick_changed(GtkEditable *editable, gpointer data) { (void)editable; quick_validate(data); }
static void quick_option(GObject *object, GParamSpec *spec, gpointer data) { (void)object; (void)spec; quick_validate(data); }
static void quick_insert(GtkButton *b, gpointer data)
{
    Tab *tab = data; guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "index"));
    guint k = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "control"));
    const char *escapes[] = {"\\r", "\\n", "\\t", "\\e"}, *hex[] = {" 0D ", " 0A ", " 09 ", " 1B "};
    GtkEditable *editable = GTK_EDITABLE(tab->quick_text[i]); gint position = gtk_editable_get_position(editable);
    const char *value = choice(tab->quick_mode[i]) ? hex[k] : escapes[k];
    gtk_editable_insert_text(editable, value, -1, &position); gtk_editable_set_position(editable, position);
}
static void quick_file_done(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GtkWidget) page = data; Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    gboolean exporting = GPOINTER_TO_INT(g_object_get_data(source, "exporting"));
    g_autoptr(GFile) file = exporting ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!tab || !file) return;
    g_autofree gchar *path = g_file_get_path(file); g_autoptr(GError) error = NULL;
    TioSessionConfig config; tio_session_config_init(&config);
    if (exporting) {
        for (guint i = 0; i < 4; i++) {
            replace(&config.quick_labels[i], entry(tab->quick_label[i])); replace(&config.quick_payloads[i], entry(tab->quick_text[i]));
            config.quick_modes[i] = choice(tab->quick_mode[i]); config.quick_endings[i] = choice(tab->quick_ending[i]);
            config.quick_crcs[i] = choice(tab->quick_crc[i]); config.quick_delays[i] = gtk_spin_button_get_value_as_int(tab->quick_delay[i]);
        }
        g_autofree gchar *encoded = tio_quick_presets_encode(&config, NULL); g_file_set_contents(path, encoded, -1, &error);
    } else {
        g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, &error);
        g_autofree gchar *contents = g_malloc(1024 * 1024 + 1); gsize length = 0;
        if (stream && g_input_stream_read_all(G_INPUT_STREAM(stream), contents, 1024 * 1024 + 1, &length, NULL, &error)
            && tio_quick_presets_decode(&config, contents, length, &error)) {
            for (guint i = 0; i < 4; i++) {
                text(tab->quick_label[i], config.quick_labels[i]); text(tab->quick_text[i], config.quick_payloads[i]);
                gtk_drop_down_set_selected(tab->quick_mode[i], config.quick_modes[i]); gtk_drop_down_set_selected(tab->quick_ending[i], config.quick_endings[i]);
                gtk_drop_down_set_selected(tab->quick_crc[i], config.quick_crcs[i]); gtk_spin_button_set_value(tab->quick_delay[i], config.quick_delays[i]);
            }
        }
    }
    gtk_label_set_text(tab->quick_status, error ? error->message : exporting ? _("Button group exported") : _("Button group loaded; Save to apply"));
    tio_session_config_clear(&config);
}
static void quick_file(Tab *tab, gboolean exporting)
{
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new(); g_object_set_data(G_OBJECT(dialog), "exporting", GINT_TO_POINTER(exporting));
    if (exporting) { gtk_file_dialog_set_initial_name(dialog, "quick-buttons.ini"); gtk_file_dialog_save(dialog, tab->quick_window, NULL, quick_file_done, g_object_ref(tab->page)); }
    else gtk_file_dialog_open(dialog, tab->quick_window, NULL, quick_file_done, g_object_ref(tab->page));
}
static void quick_import(GtkButton *b, gpointer data) { (void)b; quick_file(data, FALSE); }
static void quick_export(GtkButton *b, gpointer data) { (void)b; quick_file(data, TRUE); }
static void replay_clicked(GtkButton *b, gpointer data) { (void)b; App *app = data; tio_analyzer_open_replay(app->window); }
typedef struct { App *app; GtkTextView *text; GtkLabel *status; } HighlightEditor;
static void highlight_rules_save(GtkButton *button, gpointer data)
{
    (void)button; HighlightEditor *editor = data;
    GtkTextIter begin, end;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(editor->text);
    gtk_text_buffer_get_bounds(buffer, &begin, &end);
    g_autofree gchar *ini = gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
    g_autoptr(GError) error = NULL;
    for (guint i = 0; i < editor->app->tabs->len; ++i) {
        TioTab *tab = g_ptr_array_index(editor->app->tabs, i);
        if (!tio_highlighter_rules(tab->highlighter, ini, &error)) { gtk_label_set_text(editor->status, error->message); return; }
    }
    g_free(editor->app->settings.highlight_rules);
    editor->app->settings.highlight_rules = g_strdup(ini);
    if (!save(editor->app, &error)) gtk_label_set_text(editor->status, error->message);
    else gtk_label_set_text(editor->status, _("Saved for all sessions; applies to incoming lines"));
}
static void on_highlight_rules(GtkButton *button, gpointer data)
{
    (void)button; App *app = data;
    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), _("Custom highlight rules"));
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(app->window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window), 650, 460);
    HighlightEditor *editor = g_new0(HighlightEditor, 1); editor->app = app;
    g_object_set_data_full(G_OBJECT(window), "highlight-editor", editor, g_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(window), box);
    GtkWidget *hint = gtk_label_new(_("Add up to 32 [rule:name] sections with pattern, color and optional bold=true. Rules are case-insensitive with bounded regex matching. Clear the text to reset. Included in configuration exports."));
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE); gtk_box_append(GTK_BOX(box), hint);
    editor->text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_monospace(editor->text, TRUE);
    const char *text = app->settings.highlight_rules;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(editor->text), text && *text ? text :
        "[rule:board-alert]\npattern=OVERCURRENT|WATCHDOG\ncolor=#ff6699\nbold=true\n", -1);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(editor->text));
    gtk_box_append(GTK_BOX(box), scroll);
    editor->status = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(editor->status, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(editor->status));
    GtkWidget *save = gtk_button_new_with_label(_("Validate and save"));
    g_signal_connect(save, "clicked", G_CALLBACK(highlight_rules_save), editor);
    gtk_box_append(GTK_BOX(box), save); gtk_window_present(GTK_WINDOW(window));
}

typedef struct {
    GWeakRef parent;
    GArray *ids;
    GtkDropDown *sessions[2];
    GtkTextView *views[2];
    GtkCheckButton *follow;
    GtkEntry *filter;
    guint timer;
} SessionCompare;

static gboolean compare_refresh(gpointer data)
{
    SessionCompare *compare = data;
    if (!gtk_check_button_get_active(compare->follow)) return G_SOURCE_CONTINUE;
    g_autoptr(GtkWindow) parent = g_weak_ref_get(&compare->parent);
    App *app = parent ? g_object_get_data(G_OBJECT(parent), "tio-gui") : NULL;
    if (!app || !gtk_widget_get_visible(GTK_WIDGET(parent))) return G_SOURCE_CONTINUE;
    const char *filter = gtk_editable_get_text(GTK_EDITABLE(compare->filter));
    for (guint side = 0; side < 2; ++side) {
        guint selection = gtk_drop_down_get_selected(compare->sessions[side]);
        TioTab *tab = NULL;
        if (selection < compare->ids->len) {
            guint64 id = g_array_index(compare->ids, guint64, selection);
            for (guint i = 0; i < app->tabs->len; ++i) {
                TioTab *candidate = g_ptr_array_index(app->tabs, i);
                if (candidate->id == id) { tab = candidate; break; }
            }
        }
        GString *text = g_string_new(tab ? "" : _("Session closed. Reopen comparison to choose new sessions."));
        if (tab) {
            const GQueue *entries = tio_log_model_entries(tab->log_model);
            GList *first = entries->tail;
            for (guint i = 1; first && first->prev && i < 200; ++i) first = first->prev;
            for (GList *item = first; item; item = item->next) {
                TioLogEntry *entry = item->data;
                if (*filter && !strstr(entry->text, filter)) continue;
                g_autofree gchar *line = g_strdup_printf("%.3f  %s\n", (double)entry->time_us / 1000000.0, entry->text);
                if (text->len + strlen(line) > 128 * 1024) break;
                g_string_append(text, line);
            }
        }
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(compare->views[side]);
        GtkTextIter begin, end;
        gtk_text_buffer_get_bounds(buffer, &begin, &end);
        g_autofree gchar *old = gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
        if (!g_str_equal(old, text->str)) {
            gtk_text_buffer_set_text(buffer, text->str, (gint)text->len);
            gtk_text_buffer_get_end_iter(buffer, &end);
            gtk_text_view_scroll_to_iter(compare->views[side], &end, 0, FALSE, 0, 1);
        }
        g_string_free(text, TRUE);
    }
    return G_SOURCE_CONTINUE;
}

static void compare_free(gpointer data)
{
    SessionCompare *compare = data;
    if (compare->timer) g_source_remove(compare->timer);
    g_weak_ref_clear(&compare->parent);
    g_array_unref(compare->ids);
    g_free(compare);
}

static void on_compare_sessions(GtkButton *button, gpointer data)
{
    (void)button;
    App *app = data;
    SessionCompare *compare = g_new0(SessionCompare, 1);
    g_weak_ref_init(&compare->parent, app->window);
    compare->ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    GtkStringList *names = gtk_string_list_new(NULL);
    for (guint i = 0; i < app->tabs->len; ++i) {
        TioTab *tab = g_ptr_array_index(app->tabs, i);
        g_array_append_val(compare->ids, tab->id);
        gtk_string_list_append(names, gtk_label_get_text(tab->title));
    }
    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), _("Compare sessions"));
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(app->window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 650);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_box_append(GTK_BOX(root), gtk_label_new(_("Read-only raw log comparison · latest 200 entries per session")));
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    compare->follow = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Follow")));
    gtk_check_button_set_active(compare->follow, TRUE);
    compare->filter = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(compare->filter, _("Shared substring filter"));
    gtk_widget_set_hexpand(GTK_WIDGET(compare->filter), TRUE);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(compare->follow));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(compare->filter));
    gtk_box_append(GTK_BOX(root), bar);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    for (guint side = 0; side < 2; ++side) {
        GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        compare->sessions[side] = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(g_object_ref(names)), NULL));
        gtk_drop_down_set_selected(compare->sessions[side], MIN(side, app->tabs->len ? app->tabs->len - 1 : 0));
        gtk_box_append(GTK_BOX(column), GTK_WIDGET(compare->sessions[side]));
        compare->views[side] = GTK_TEXT_VIEW(gtk_text_view_new());
        gtk_text_view_set_editable(compare->views[side], FALSE);
        gtk_text_view_set_monospace(compare->views[side], TRUE);
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(compare->views[side]));
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_widget_set_hexpand(scroll, TRUE);
        gtk_widget_set_size_request(scroll, 300, 200);
        gtk_box_append(GTK_BOX(column), scroll);
        if (!side) gtk_paned_set_start_child(GTK_PANED(paned), column);
        else gtk_paned_set_end_child(GTK_PANED(paned), column);
    }
    g_object_unref(names);
    gtk_box_append(GTK_BOX(root), paned);
    gtk_window_set_child(GTK_WINDOW(window), root);
    g_object_set_data_full(G_OBJECT(window), "session-compare", compare, compare_free);
    compare->timer = g_timeout_add(250, compare_refresh, compare);
    compare_refresh(compare);
    gtk_window_present(GTK_WINDOW(window));
}

static void profiles_refresh(Tab *tab)
{
    if (!tab->saved_profiles) return;
    GtkStringList *names = gtk_string_list_new(NULL);
    for (guint i = 0; i < tab->app->settings.profiles->len; i++) {
        TioProfile *profile = g_ptr_array_index(tab->app->settings.profiles, i); gtk_string_list_append(names, profile->name);
    }
    gtk_drop_down_set_model(tab->saved_profiles, G_LIST_MODEL(names)); g_object_unref(names);
}
static void profiles_selected(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)spec; Tab *tab = data;
    GtkStringObject *selected = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(object));
    if (selected && tab->profile) text(tab->profile, gtk_string_object_get_string(selected));
}
static void profile_delete(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data; tio_settings_remove_profile(&tab->app->settings, entry(tab->profile));
    for (guint i = 0; i < tab->app->tabs->len; i++) profiles_refresh(g_ptr_array_index(tab->app->tabs, i));
    g_autoptr(GError) error = NULL; if (!save(tab->app, &error)) set_status(tab, error->message);
}
static gboolean import_settings(App *app, const char *path, GError **error)
{
    for (guint i = 0; i < app->tabs->len; i++) {
        Tab *tab = g_ptr_array_index(app->tabs, i);
        if (tab->serial || (tab->capture && !tio_capture_finished(tab->capture))) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Disconnect all sessions before importing settings"); return FALSE;
        }
    }
    GStatBuf info;
    if (!path || g_stat(path, &info) || info.st_size > 2 * 1024 * 1024) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Choose a settings file up to 2 MiB"); return FALSE;
    }
    TioSettings loaded; tio_settings_init(&loaded);
    if (!tio_settings_load_from_file(&loaded, path, error)) { tio_settings_clear(&loaded); return FALSE; }
    g_autoptr(GtkTextBuffer) validation = gtk_text_buffer_new(NULL);
    TioHighlighter *highlighter = tio_highlighter_new(validation);
    gboolean valid = tio_highlighter_rules(highlighter, loaded.highlight_rules, error); tio_highlighter_free(highlighter);
    if (!valid) { tio_settings_clear(&loaded); return FALSE; }
    loaded.native_tools_version = 1;
    if (g_strcmp0(loaded.language, "en")) replace(&loaded.language, "zh_CN");
    g_autofree gchar *backup = g_strconcat(app->settings_path, ".before-import", NULL);
    /* The newest stored slot may be .bk, and widgets can be newer than either
     * slot. Back up the complete current snapshot as a single importable file. */
    snapshot_settings(app);
    if (!ensure_settings_directory(app, error) || !tio_settings_save_to_file(&app->settings, backup, error)) { tio_settings_clear(&loaded); return FALSE; }
    /* Commit before replacing the workspace so a damaged or unwritable store
     * leaves the current tabs intact and reports the import as failed. */
    if (!tio_settings_save_to_store(&loaded, app->settings_path, error)) { tio_settings_clear(&loaded); return FALSE; }
    while (app->tabs->len) {
        guint i = app->tabs->len - 1; Tab *tab = g_ptr_array_index(app->tabs, i);
        gint page = gtk_notebook_page_num(app->notebook, tab->page);
        g_ptr_array_remove_index(app->tabs, i); tab_free(tab); gtk_notebook_remove_page(app->notebook, page);
    }
    tio_settings_clear(&app->settings); app->settings = loaded;
    native_language(app->settings.language); native_theme(app);
    app->translating = TRUE;
    gtk_drop_down_set_selected(app->theme, !g_strcmp0(app->settings.theme, "dark") ? 2 : !g_strcmp0(app->settings.theme, "light") ? 1 : 0);
    gtk_drop_down_set_selected(app->language, !g_strcmp0(app->settings.language, "en"));
    gtk_spin_button_set_value(app->font_size, app->settings.font_size);
    gtk_check_button_set_active(app->restore_tabs, app->settings.restore_tabs);
    app->translating = FALSE;
    for (guint i = 0; i < app->settings.tab_configs->len; i++) new_tab(app, g_ptr_array_index(app->settings.tab_configs, i));
    if (!app->tabs->len) new_tab(app, &app->settings.defaults);
    translate_widgets(GTK_WIDGET(app->window)); return TRUE;
}
static void settings_file_done(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GtkWindow) window = data; App *app = g_object_get_data(G_OBJECT(window), "tio-gui");
    guint mode = GPOINTER_TO_UINT(g_object_get_data(source, "mode"));
    g_autoptr(GFile) file = mode ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!app || app->closing || !file) return;
    g_autofree gchar *path = g_file_get_path(file); g_autoptr(GError) error = NULL;
    gboolean ok = FALSE;
    if (!mode) ok = import_settings(app, path, &error);
    else {
        snapshot_settings(app);
        ok = mode == 2 ? tio_settings_export_portable(&app->settings, path, &error)
            : tio_settings_save_to_file(&app->settings, path, &error);
    }
    if (!ok) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("%s", error ? error->message : _("Choose a local file"));
        gtk_alert_dialog_show(alert, app->window);
    }
}
static void settings_file(GtkButton *b, gpointer data)
{
    App *app = data; guint mode = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "mode"));
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new(); g_object_set_data(G_OBJECT(dialog), "mode", GUINT_TO_POINTER(mode));
    if (mode) { gtk_file_dialog_set_initial_name(dialog, mode == 2 ? "tio-gui-portable.ini" : "tio-gui.ini"); gtk_file_dialog_save(dialog, app->window, NULL, settings_file_done, g_object_ref(app->window)); }
    else gtk_file_dialog_open(dialog, app->window, NULL, settings_file_done, g_object_ref(app->window));
}
static void font_changed(GtkSpinButton *spin, gpointer data)
{
    App *app = data; if (app->translating) return;
    app->settings.font_size = gtk_spin_button_get_value_as_int(spin);
    for (guint i = 0; i < app->tabs->len; i++) {
        Tab *tab = g_ptr_array_index(app->tabs, i); tio_console_font(GTK_WIDGET(tab->view), app->settings.font_size);
        tio_console_font(GTK_WIDGET(tio_terminal_view(tab->terminal)), app->settings.font_size);
    }
    g_autoptr(GError) error = NULL; if (!save(app, &error)) g_warning("%s", error->message);
}
static GtkWidget *tools_card(GtkWidget *box, const char *title)
{
    GtkWidget *label = gtk_label_new(title); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "settings-section-title"); append(box, label);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_widget_add_css_class(card, "settings-card"); append(box, card); return card;
}
static void tools_preferences(GtkWidget *box, App *app)
{
    GtkWidget *card = tools_card(box, _("General")), *r = row(); append(card, r);
    GtkWidget *caption = gtk_label_new(_("Font size")); gtk_label_set_xalign(GTK_LABEL(caption), 0); gtk_widget_set_hexpand(caption, TRUE); append(r, caption);
    app->font_size = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(6, 40, 1)); gtk_spin_button_set_value(app->font_size, app->settings.font_size);
    append(r, GTK_WIDGET(app->font_size)); g_signal_connect(app->font_size, "value-changed", G_CALLBACK(font_changed), app);
    app->restore_tabs = check(card, _("Reopen sessions on startup"), app->settings.restore_tabs);
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->restore_tabs), _("Reopen the tabs that were open last time, without connecting"));
    card = tools_card(box, _("Backup")); r = row(); append(card, r);
    GtkWidget *b = button(r, _("Export settings…"), G_CALLBACK(settings_file), app); g_object_set_data(G_OBJECT(b), "mode", GUINT_TO_POINTER(1));
    gtk_widget_set_hexpand(b, TRUE); gtk_widget_set_hexpand(button(r, _("Import settings…"), G_CALLBACK(settings_file), app), TRUE);
    b = button(card, _("Export portable settings…"), G_CALLBACK(settings_file), app); g_object_set_data(G_OBJECT(b), "mode", GUINT_TO_POINTER(2));
    gtk_widget_set_tooltip_text(b, _("Profiles, buttons and sequences; excludes local paths, device identities and send history"));
    card = tools_card(box, _("About"));
    g_autofree gchar *version = g_strdup_printf(_("Version %s"), TIO_GUI_VERSION);
    GtkWidget *label = gtk_label_new(version); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_widget_add_css_class(label, "settings-version");
    g_object_set_data(G_OBJECT(label), "user-content", GINT_TO_POINTER(1)); append(card, label);
    tool_hint(card, _("A lightweight, reliable GUI for tio"));
}
/* Window-wide tools, the same entries as the Linux Tools menu minus network backends. */
static GtkWidget *tools_menu(App *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2); tio_ui_margins(box, 6);
    struct { const char *label; GCallback callback; } items[] = {
        {_("Compare sessions…"), G_CALLBACK(on_compare_sessions)},
        {_("Open recording / replay…"), G_CALLBACK(replay_clicked)},
        {_("Custom highlight rules…"), G_CALLBACK(on_highlight_rules)},
    };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *b = button(box, items[i].label, items[i].callback, app);
        gtk_widget_add_css_class(b, "flat"); gtk_widget_add_css_class(b, "menu-item");
        gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(b))), 0);
    }
    return box;
}
