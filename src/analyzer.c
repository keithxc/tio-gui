/* SPDX-License-Identifier: GPL-3.0-only */
#include "analyzer.h"
#include "capture.h"
#include <libintl.h>
#include <math.h>
#include <string.h>
#define _(s) gettext(s)

typedef struct { double x, y; } Point;
typedef struct {
    GtkWidget *window;
    TioLogModel *model;
    gboolean owns_model;
    TioReplay *replay;
    GtkWidget *replay_controls;
    GtkCheckButton *replay_pause, *replay_hex;
    GtkSpinButton *replay_speed;
    GtkLabel *replay_status;
    guint load_serial;
    GtkEntry *filter_entry, *fields_entry, *extract_entry;
    GtkCheckButton *regex, *sensitive, *follow, *levels[TIO_LOG_LEVELS], *curves[3];
    GtkSpinButton *point_limit;
    GtkTextView *list;
    GtkBox *details;
    GtkLabel *status;
    GtkDrawingArea *plot;
    GArray *ids, *points[3];
    TioLogFilter *filter, *extractor;
    guint timer;
    guint64 revision;
    gboolean rebuild, updating;
    gchar *field_names[3];
} Analyzer;

static void draw_plot(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)area;
    Analyzer *view = data;
    double minimum = INFINITY, maximum = -INFINITY, first = INFINITY, last = -INFINITY;
    for (guint k = 0; k < 3; ++k) {
        if (!gtk_check_button_get_active(view->curves[k])) continue;
        for (guint i = 0; i < view->points[k]->len; ++i) {
            Point p = g_array_index(view->points[k], Point, i);
            minimum = MIN(minimum, p.y); maximum = MAX(maximum, p.y);
            first = MIN(first, p.x); last = MAX(last, p.x);
        }
    }
    cairo_set_source_rgb(cr, .1, .12, .15); cairo_paint(cr);
    cairo_set_source_rgb(cr, .65, .68, .72);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    if (!isfinite(minimum)) {
        cairo_move_to(cr, 16, 28); cairo_show_text(cr, _("Select numeric fields to plot")); return;
    }
    if (maximum == minimum) { maximum += .5; minimum -= .5; }
    if (last == first) last += 1;
    char label[80];
    g_snprintf(label, sizeof label, "%.5g", maximum);
    cairo_move_to(cr, 6, 18); cairo_show_text(cr, label);
    g_snprintf(label, sizeof label, "%.5g", minimum);
    cairo_move_to(cr, 6, height - 24); cairo_show_text(cr, label);
    g_snprintf(label, sizeof label, "%.3g s", last - first);
    cairo_move_to(cr, MAX(80, width - 90), height - 5); cairo_show_text(cr, label);
    const double colors[3][3] = {{.3,.7,1}, {1,.65,.25}, {.4,.9,.55}};
    for (guint k = 0; k < 3; ++k) {
        if (!gtk_check_button_get_active(view->curves[k])) continue;
        cairo_set_source_rgb(cr, colors[k][0], colors[k][1], colors[k][2]);
        cairo_set_line_width(cr, 1.5);
        for (guint i = 0; i < view->points[k]->len; ++i) {
            Point p = g_array_index(view->points[k], Point, i);
            double x = 65 + (p.x - first) / (last - first) * MAX(1, width - 80);
            double y = 15 + (maximum - p.y) / (maximum - minimum) * MAX(1, height - 45);
            if (!i) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }
}

static void append_json(GtkBox *box, const char *name, JsonNode *node, guint depth, guint *budget)
{
    if (!*budget || depth > 16) return;
    --*budget;
    if (JSON_NODE_HOLDS_OBJECT(node) || JSON_NODE_HOLDS_ARRAY(node)) {
        GtkWidget *expander = gtk_expander_new(name);
        gtk_expander_set_expanded(GTK_EXPANDER(expander), depth < 2);
        GtkWidget *children = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        gtk_widget_set_margin_start(children, 12);
        gtk_expander_set_child(GTK_EXPANDER(expander), children);
        gtk_box_append(box, expander);
        if (JSON_NODE_HOLDS_OBJECT(node)) {
            JsonObject *object = json_node_get_object(node);
            GList *members = json_object_get_members(object);
            for (GList *item = members; item && *budget; item = item->next)
                append_json(GTK_BOX(children), item->data, json_object_get_member(object, item->data), depth + 1, budget);
            g_list_free(members);
        } else {
            JsonArray *array = json_node_get_array(node);
            for (guint i = 0; i < json_array_get_length(array) && *budget; ++i) {
                g_autofree gchar *index = g_strdup_printf("[%u]", i);
                append_json(GTK_BOX(children), index, json_array_get_element(array, i), depth + 1, budget);
            }
        }
        return;
    }
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, node);
    g_autofree gchar *json = json_generator_to_data(generator, NULL);
    char number[80];
    if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_DOUBLE) {
        g_ascii_formatd(number, sizeof number, "%.15g", json_node_get_double(node));
        g_free(json); json = g_strdup(number);
    }
    const char *value = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : json;
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *key = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(key), 0);
    gtk_widget_add_css_class(key, "heading");
    GtkWidget *label = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_box_append(GTK_BOX(row), key); gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(box, row);
}

static void select_entry(GtkGestureClick *gesture, int presses, double x, double y, gpointer data)
{
    (void)gesture; (void)presses; (void)x; (void)y;
    Analyzer *view = data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->list);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
    gint line = gtk_text_iter_get_line(&iter);
    if (line < 0 || (guint)line >= view->ids->len) return;
    guint64 id = g_array_index(view->ids, guint64, (guint)line);
    const GQueue *entries = tio_log_model_entries(view->model);
    for (GList *item = entries->head; item; item = item->next) {
        TioLogEntry *entry = item->data;
        if (entry->id != id) continue;
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(GTK_WIDGET(view->details)))) gtk_box_remove(view->details, child);
        GtkWidget *raw = gtk_label_new(entry->text);
        gtk_label_set_wrap(GTK_LABEL(raw), TRUE); gtk_label_set_selectable(GTK_LABEL(raw), TRUE);
        gtk_box_append(view->details, raw);
        g_autoptr(JsonNode) fields = tio_log_entry_fields(entry);
        guint budget = 256;
        append_json(view->details, _("Fields"), fields, 0, &budget);
        break;
    }
    gtk_check_button_set_active(view->follow, FALSE);
}

static void rebuild(Analyzer *view)
{
    const GQueue *entries = tio_log_model_entries(view->model);
    g_autoptr(GPtrArray) matches = g_ptr_array_new();
    for (GList *item = entries->head; item; item = item->next)
        if (tio_log_filter_matches(view->filter, item->data)) g_ptr_array_add(matches, item->data);
    g_autoptr(GString) text = g_string_new(NULL);
    g_array_set_size(view->ids, 0);
    guint start = matches->len > 1000 ? matches->len - 1000 : 0;
    for (guint i = start; i < matches->len; ++i) {
        TioLogEntry *entry = g_ptr_array_index(matches, i);
        g_string_append_printf(text, "%s\n", entry->text);
        g_array_append_val(view->ids, entry->id);
    }
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->list);
    view->updating = TRUE;
    gtk_text_buffer_set_text(buffer, text->str, (gint)text->len);
    if (gtk_check_button_get_active(view->follow)) {
        GtkTextIter end; gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_view_scroll_to_iter(view->list, &end, 0, FALSE, 0, 0);
    }
    view->updating = FALSE;
    guint limit = (guint)gtk_spin_button_get_value_as_int(view->point_limit);
    for (guint k = 0; k < 3; ++k) {
        g_array_set_size(view->points[k], 0);
        if (!view->field_names[k] || !*view->field_names[k]) continue;
        for (gint i = (gint)matches->len - 1; i >= 0 && view->points[k]->len < limit; --i) {
            TioLogEntry *entry = g_ptr_array_index(matches, (guint)i);
            double value;
            gboolean found = view->extractor ? tio_log_filter_number(view->extractor, entry, view->field_names[k], &value)
                : tio_log_entry_number(entry, view->field_names[k], &value);
            if (found) {
                Point point = {(double)entry->time_us / 1000000.0, value};
                g_array_append_val(view->points[k], point);
            }
        }
    }
    gtk_widget_queue_draw(GTK_WIDGET(view->plot));
    g_autofree gchar *status = g_strdup_printf(_("%u retained · %u matching · showing latest %u · regex limits: %" G_GUINT64_FORMAT),
        entries->length, matches->len, view->ids->len, tio_log_filter_limited(view->filter));
    gtk_label_set_text(view->status, status);
    view->revision = tio_log_model_revision(view->model);
    view->rebuild = FALSE;
}

static gboolean tick(gpointer data)
{
    Analyzer *view = data;
    if (view->replay) {
        g_autofree gchar *progress = g_strdup_printf(tio_replay_finished(view->replay)
            ? _("Replay complete: %u/%u events") : _("Replay: %u/%u events"),
            tio_replay_position(view->replay), tio_replay_count(view->replay));
        gtk_label_set_text(view->replay_status, progress);
    }
    for (guint i = 0; i < TIO_LOG_LEVELS; ++i) {
        g_autofree gchar *label = g_strdup_printf("%s: %" G_GUINT64_FORMAT,
            tio_log_level_name((TioLogLevel)i), tio_log_model_count(view->model, (TioLogLevel)i));
        gtk_check_button_set_label(view->levels[i], label);
    }
    if (view->rebuild || (gtk_check_button_get_active(view->follow) && view->revision != tio_log_model_revision(view->model))) rebuild(view);
    return G_SOURCE_CONTINUE;
}

static void apply_filter(GtkWidget *widget, gpointer data)
{
    (void)widget;
    Analyzer *view = data;
    guint mask = 0;
    for (guint i = 0; i < TIO_LOG_LEVELS; ++i) if (gtk_check_button_get_active(view->levels[i])) mask |= 1u << i;
    g_autoptr(GError) error = NULL;
    TioLogFilter *filter = tio_log_filter_new(gtk_editable_get_text(GTK_EDITABLE(view->filter_entry)),
        gtk_check_button_get_active(view->regex), gtk_check_button_get_active(view->sensitive), mask, &error);
    if (!filter) { gtk_label_set_text(view->status, error->message); return; }
    TioLogFilter *extractor = NULL;
    const char *pattern = gtk_editable_get_text(GTK_EDITABLE(view->extract_entry));
    if (*pattern) {
        extractor = tio_log_filter_new(pattern, TRUE, TRUE, 127, &error);
        if (!extractor) { tio_log_filter_free(filter); gtk_label_set_text(view->status, error->message); return; }
    }
    tio_log_filter_free(view->filter); view->filter = filter;
    tio_log_filter_free(view->extractor); view->extractor = extractor;
    g_auto(GStrv) fields = g_strsplit(gtk_editable_get_text(GTK_EDITABLE(view->fields_entry)), ",", 4);
    guint count = g_strv_length(fields);
    for (guint i = 0; i < 3; ++i) {
        g_free(view->field_names[i]);
        view->field_names[i] = g_strdup(i < count ? g_strstrip(fields[i]) : "");
        gtk_check_button_set_label(view->curves[i], *view->field_names[i] ? view->field_names[i] : "—");
    }
    view->rebuild = TRUE;
}

static void curve_changed(GtkCheckButton *button, gpointer data)
{
    (void)button;
    gtk_widget_queue_draw(GTK_WIDGET(((Analyzer *)data)->plot));
}
static void selection_changed(GtkTextBuffer *buffer, const GParamSpec *spec, gpointer data)
{
    (void)spec;
    Analyzer *view = data;
    if (!view->updating && gtk_text_buffer_get_has_selection(buffer)) gtk_check_button_set_active(view->follow, FALSE);
}

static void export_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    GWeakRef *weak = data;
    g_autoptr(GObject) window = g_weak_ref_get(weak);
    g_weak_ref_clear(weak); g_free(weak);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!window || !gtk_widget_get_visible(GTK_WIDGET(window))) return;
    Analyzer *view = g_object_get_data(window, "analyzer");
    if (!file) return;
    g_autofree gchar *path = g_file_get_path(file);
    if (!path) { gtk_label_set_text(view->status, _("Choose a local CSV file")); return; }
    g_autofree gchar *csv = tio_log_model_csv(view->model, view->filter);
    if (!g_file_set_contents(path, csv, -1, &error)) gtk_label_set_text(view->status, error->message);
    else gtk_label_set_text(view->status, _("Filtered CSV exported"));
}
static void export_csv(GtkButton *button, gpointer data)
{
    (void)button;
    Analyzer *view = data;
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_initial_name(dialog, "serial-analysis.csv");
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, view->window);
    gtk_file_dialog_save(dialog, GTK_WINDOW(view->window), NULL, export_finished, weak);
}
static void clear_analysis(GtkButton *button, gpointer data)
{
    (void)button;
    Analyzer *view = data;
    tio_log_model_clear(view->model);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(view->details)))) gtk_box_remove(view->details, child);
    view->rebuild = TRUE;
}
static void free_analyzer(gpointer data)
{
    Analyzer *view = data;
    if (view->timer) g_source_remove(view->timer);
    tio_replay_free(view->replay);
    if (view->owns_model) tio_log_model_free(view->model);
    tio_log_filter_free(view->filter); tio_log_filter_free(view->extractor);
    g_array_unref(view->ids);
    for (guint i = 0; i < 3; ++i) { g_array_unref(view->points[i]); g_free(view->field_names[i]); }
    g_free(view);
}

/* Recordings are loaded off the GTK thread and replayed only into a fresh model. */
typedef struct { GWeakRef window; gchar *path; guint serial; Analyzer *view; } ReplayLoad;
static void replay_event(TioCaptureKind kind, const guint8 *bytes, gsize length, gint64 time, gpointer data)
{
    Analyzer *view = data;
    if (kind == TIO_CAPTURE_RX && !gtk_check_button_get_active(view->replay_hex)) {
        tio_log_model_feed(view->model, bytes, length, time);
        return;
    }
    if (kind == TIO_CAPTURE_RX || kind == TIO_CAPTURE_TX || kind == TIO_CAPTURE_INPUT) {
        g_autoptr(GString) text = g_string_new(kind == TIO_CAPTURE_RX ? "RX " : kind == TIO_CAPTURE_TX ? "TX " : "INPUT ");
        for (gsize i = 0; i < MIN(length, 1024u); ++i) g_string_append_printf(text, "%02X ", bytes[i]);
        if (length > 1024) g_string_append_printf(text, "… (%zu bytes total)", length);
        if (kind == TIO_CAPTURE_RX) {
            g_string_append_c(text, '\n');
            tio_log_model_feed(view->model, (const guint8 *)text->str, text->len, time);
        } else tio_log_model_command(view->model, text->str, time);
    } else {
        const char *prefix = kind == TIO_CAPTURE_CONNECT ? "CONNECTED " : kind == TIO_CAPTURE_DISCONNECT ? "DISCONNECTED " : "PARAMETERS ";
        g_autofree gchar *valid = g_utf8_make_valid((const char *)bytes, (gssize)length);
        g_autofree gchar *text = g_strconcat(prefix, valid, NULL);
        tio_log_model_command(view->model, text, time);
    }
}
static void replay_load_free(gpointer data)
{
    ReplayLoad *load = data; g_weak_ref_clear(&load->window); g_free(load->path); g_free(load);
}
static void replay_load_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    (void)source; (void)cancellable;
    ReplayLoad *load = task_data;
    g_autoptr(GError) error = NULL;
    TioReplay *replay = tio_replay_load(load->path, replay_event, load->view, &error);
    if (!replay) g_task_return_error(task, g_steal_pointer(&error));
    else g_task_return_pointer(task, replay, (GDestroyNotify)tio_replay_free);
}
static void replay_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; (void)data;
    ReplayLoad *load = g_task_get_task_data(G_TASK(result));
    g_autoptr(GObject) window = g_weak_ref_get(&load->window);
    g_autoptr(GError) error = NULL;
    TioReplay *replay = g_task_propagate_pointer(G_TASK(result), &error);
    if (!window || !gtk_widget_get_visible(GTK_WIDGET(window))) { tio_replay_free(replay); return; }
    Analyzer *view = g_object_get_data(window, "analyzer");
    if (load->serial != view->load_serial) { tio_replay_free(replay); return; }
    if (!replay) { gtk_label_set_text(view->replay_status, error->message); return; }
    tio_replay_free(view->replay); view->replay = replay;
    tio_log_model_clear(view->model); view->rebuild = TRUE;
    tio_replay_speed(replay, gtk_spin_button_get_value(view->replay_speed));
    tio_replay_pause(replay, gtk_check_button_get_active(view->replay_pause));
    gtk_label_set_text(view->replay_status, _("Loaded; uncheck Pause replay to play"));
}
static void replay_file_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    GWeakRef *weak = data;
    g_autoptr(GObject) window = g_weak_ref_get(weak);
    g_weak_ref_clear(weak); g_free(weak);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!window || !gtk_widget_get_visible(GTK_WIDGET(window)) || !file) return;
    Analyzer *view = g_object_get_data(window, "analyzer");
    g_autofree gchar *path = g_file_get_path(file);
    if (!path) { gtk_label_set_text(view->replay_status, _("Choose a local recording")); return; }
    ReplayLoad *load = g_new0(ReplayLoad, 1);
    g_weak_ref_init(&load->window, window); load->path = g_steal_pointer(&path);
    load->serial = ++view->load_serial; load->view = view;
    gtk_label_set_text(view->replay_status, _("Loading recording…"));
    g_autoptr(GTask) task = g_task_new(NULL, NULL, replay_loaded, NULL);
    g_task_set_task_data(task, load, replay_load_free);
    g_task_run_in_thread(task, replay_load_worker);
}
static void choose_replay(Analyzer *view)
{
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Open recording or retained part"));
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, view->window);
    gtk_file_dialog_open(dialog, GTK_WINDOW(view->window), NULL, replay_file_finished, weak);
}
static void open_replay(GtkButton *button, gpointer data)
{
    (void)button;
    Analyzer *view = data;
    if (view->owns_model) choose_replay(view);
    else {
        GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(view->window));
        tio_analyzer_open_replay(parent ? parent : GTK_WINDOW(view->window));
    }
}
static void replay_controls_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    Analyzer *view = data;
    tio_replay_pause(view->replay, gtk_check_button_get_active(view->replay_pause));
    tio_replay_speed(view->replay, gtk_spin_button_get_value(view->replay_speed));
}

GtkWidget *tio_analyzer_new(GtkWindow *parent, TioLogModel *model)
{
    Analyzer *view = g_new0(Analyzer, 1);
    view->model = model; view->rebuild = TRUE;
    view->ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    for (guint i = 0; i < 3; ++i) view->points[i] = g_array_new(FALSE, FALSE, sizeof(Point));
    view->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(view->window), _("Serial log analysis"));
    gtk_window_set_transient_for(GTK_WINDOW(view->window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(view->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(view->window), 1120, 760);
    g_object_set_data_full(G_OBJECT(view->window), "analyzer", view, free_analyzer);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root, 8); gtk_widget_set_margin_bottom(root, 8);
    gtk_widget_set_margin_start(root, 8); gtk_widget_set_margin_end(root, 8);
    gtk_window_set_child(GTK_WINDOW(view->window), root);
    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    view->filter_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(view->filter_entry, _("Filter received lines"));
    gtk_widget_set_hexpand(GTK_WIDGET(view->filter_entry), TRUE);
    view->regex = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Regex")));
    view->sensitive = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Case sensitive")));
    view->follow = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Follow")));
    gtk_check_button_set_active(view->follow, TRUE);
    GtkWidget *apply = gtk_button_new_with_label(_("Apply"));
    GtkWidget *export = gtk_button_new_with_label(_("Export filtered CSV…"));
    gtk_box_append(GTK_BOX(filters), GTK_WIDGET(view->filter_entry));
    gtk_box_append(GTK_BOX(filters), GTK_WIDGET(view->regex));
    gtk_box_append(GTK_BOX(filters), GTK_WIDGET(view->sensitive));
    gtk_box_append(GTK_BOX(filters), apply); gtk_box_append(GTK_BOX(filters), GTK_WIDGET(view->follow));
    gtk_box_append(GTK_BOX(filters), export); gtk_box_append(GTK_BOX(root), filters);
    GtkWidget *recording = gtk_button_new_with_label(_("Open recording…"));
    g_signal_connect(recording, "clicked", G_CALLBACK(open_replay), view);
    gtk_box_append(GTK_BOX(filters), recording);
    view->replay_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    view->replay_pause = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Pause replay")));
    gtk_check_button_set_active(view->replay_pause, TRUE);
    view->replay_hex = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("RX as HEX")));
    view->replay_speed = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(.1, 32, .1));
    gtk_spin_button_set_value(view->replay_speed, 1);
    gtk_spin_button_set_digits(view->replay_speed, 1);
    view->replay_status = GTK_LABEL(gtk_label_new(_("Playback never sends data to a device")));
    gtk_box_append(GTK_BOX(view->replay_controls), GTK_WIDGET(view->replay_pause));
    gtk_box_append(GTK_BOX(view->replay_controls), GTK_WIDGET(view->replay_hex));
    gtk_box_append(GTK_BOX(view->replay_controls), gtk_label_new(_("Speed ×")));
    gtk_box_append(GTK_BOX(view->replay_controls), GTK_WIDGET(view->replay_speed));
    gtk_box_append(GTK_BOX(view->replay_controls), GTK_WIDGET(view->replay_status));
    g_signal_connect(view->replay_pause, "toggled", G_CALLBACK(replay_controls_changed), view);
    g_signal_connect(view->replay_speed, "value-changed", G_CALLBACK(replay_controls_changed), view);
    gtk_box_append(GTK_BOX(root), view->replay_controls);
    gtk_widget_set_visible(view->replay_controls, FALSE);
    g_signal_connect(apply, "clicked", G_CALLBACK(apply_filter), view);
    g_signal_connect(view->filter_entry, "activate", G_CALLBACK(apply_filter), view);
    g_signal_connect(export, "clicked", G_CALLBACK(export_csv), view);
    GtkWidget *levels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (guint i = 0; i < TIO_LOG_LEVELS; ++i) {
        view->levels[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(tio_log_level_name((TioLogLevel)i)));
        gtk_check_button_set_active(view->levels[i], TRUE);
        gtk_box_append(GTK_BOX(levels), GTK_WIDGET(view->levels[i]));
        g_signal_connect(view->levels[i], "toggled", G_CALLBACK(apply_filter), view);
    }
    GtkWidget *clear = gtk_button_new_with_label(_("Clear analysis"));
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_analysis), view);
    gtk_box_append(GTK_BOX(levels), clear);
    gtk_box_append(GTK_BOX(root), levels);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE); gtk_paned_set_position(GTK_PANED(paned), 720);
    view->list = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(view->list, FALSE); gtk_text_view_set_monospace(view->list, TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(view->list));
    gtk_paned_set_start_child(GTK_PANED(paned), scroll);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "released", G_CALLBACK(select_entry), view);
    gtk_widget_add_controller(GTK_WIDGET(view->list), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(gtk_text_view_get_buffer(view->list), "notify::has-selection", G_CALLBACK(selection_changed), view);
    view->details = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_box_append(view->details, gtk_label_new(_("Click a log line to inspect and copy its fields")));
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(view->details));
    gtk_widget_set_size_request(scroll, 260, -1);
    gtk_paned_set_end_child(GTK_PANED(paned), scroll); gtk_box_append(GTK_BOX(root), paned);
    GtkWidget *plot_options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    view->fields_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(view->fields_entry, _("Plot fields: temp,voltage or col1,col2"));
    gtk_widget_set_hexpand(GTK_WIDGET(view->fields_entry), TRUE);
    view->extract_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(view->extract_entry, _("Optional regex with named captures"));
    gtk_widget_set_hexpand(GTK_WIDGET(view->extract_entry), TRUE);
    view->point_limit = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(10, 5000, 100));
    gtk_spin_button_set_value(view->point_limit, 500);
    gtk_widget_set_tooltip_text(GTK_WIDGET(view->point_limit), _("Maximum points per curve"));
    gtk_box_append(GTK_BOX(plot_options), GTK_WIDGET(view->fields_entry));
    gtk_box_append(GTK_BOX(plot_options), GTK_WIDGET(view->extract_entry));
    gtk_box_append(GTK_BOX(plot_options), GTK_WIDGET(view->point_limit));
    GtkWidget *plot_apply = gtk_button_new_with_label(_("Plot"));
    g_signal_connect(plot_apply, "clicked", G_CALLBACK(apply_filter), view);
    gtk_box_append(GTK_BOX(plot_options), plot_apply);
    gtk_box_append(GTK_BOX(root), plot_options);
    GtkWidget *legend = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (guint i = 0; i < 3; ++i) {
        view->curves[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("—"));
        gtk_check_button_set_active(view->curves[i], TRUE);
        gtk_box_append(GTK_BOX(legend), GTK_WIDGET(view->curves[i]));
        g_signal_connect(view->curves[i], "toggled", G_CALLBACK(curve_changed), view);
    }
    gtk_box_append(GTK_BOX(root), legend);
    view->plot = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_widget_set_size_request(GTK_WIDGET(view->plot), -1, 180);
    gtk_drawing_area_set_draw_func(view->plot, draw_plot, view, NULL);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(view->plot));
    view->status = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(view->status, TRUE);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(view->status));
    view->timer = g_timeout_add(250, tick, view);
    gtk_window_present(GTK_WINDOW(view->window));
    return view->window;
}

GtkWidget *tio_analyzer_open_replay(GtkWindow *parent)
{
    TioLogModel *model = tio_log_model_new();
    GtkWidget *window = tio_analyzer_new(parent, model);
    Analyzer *view = g_object_get_data(G_OBJECT(window), "analyzer");
    view->owns_model = TRUE;
    gtk_window_set_title(GTK_WINDOW(window), _("Recording playback"));
    gtk_widget_set_visible(view->replay_controls, TRUE);
    choose_replay(view);
    return window;
}
