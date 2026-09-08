/* SPDX-License-Identifier: GPL-3.0-only */
#include "plugin_ui.h"
#include "plugin.h"
#include <libintl.h>
#include <string.h>
#define _(s) gettext(s)
typedef struct {
    GtkWidget *window, *run;
    GtkTextView *source, *output;
    GtkDropDown *language;
    gchar *input;
    GCancellable *cancel;
} Editor;
static void editor_free(gpointer data)
{
    Editor *editor = data;
    if (editor->cancel) g_cancellable_cancel(editor->cancel);
    g_clear_object(&editor->cancel); g_free(editor->input); g_free(editor);
}
static void completed(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; GWeakRef *weak = data;
    g_autoptr(GObject) window = g_weak_ref_get(weak);
    g_weak_ref_clear(weak); g_free(weak);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *output = tio_plugin_run_finish(result, &error);
    if (!window || !gtk_widget_get_visible(GTK_WIDGET(window))) return;
    Editor *editor = g_object_get_data(window, "plugin-editor");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(editor->output), output ? output : error->message, -1);
    gtk_widget_set_sensitive(editor->run, TRUE); g_clear_object(&editor->cancel);
}
static gchar *source_text(Editor *editor)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(editor->source);
    GtkTextIter begin, end; gtk_text_buffer_get_bounds(buffer, &begin, &end);
    return gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
}
static void run(GtkButton *button, gpointer data)
{
    (void)button; Editor *editor = data;
    if (editor->cancel) return;
    g_autofree gchar *source = source_text(editor);
    editor->cancel = g_cancellable_new(); gtk_widget_set_sensitive(editor->run, FALSE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(editor->output), _("Running isolated plugin…"), -1);
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, editor->window);
    tio_plugin_run_async(gtk_drop_down_get_selected(editor->language), source, editor->input, editor->cancel, completed, weak);
}
typedef struct { GWeakRef window; gboolean save; gchar *text; } FileRequest;
static void file_selected(GObject *source, GAsyncResult *result, gpointer data)
{
    FileRequest *request = data;
    g_autoptr(GObject) window = g_weak_ref_get(&request->window);
    gboolean save = request->save; g_autofree gchar *text = request->text;
    g_weak_ref_clear(&request->window); g_free(request);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = save ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!window || !gtk_widget_get_visible(GTK_WIDGET(window)) || !file) return;
    Editor *editor = g_object_get_data(window, "plugin-editor");
    gboolean ok = FALSE;
    if (save) ok = g_file_replace_contents(file, text, strlen(text), NULL, FALSE, G_FILE_CREATE_PRIVATE, NULL, NULL, &error);
    else {
        g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, NULL, &error);
        if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR) {
            g_autoptr(GFileInputStream) input = g_file_read(file, NULL, &error);
            gchar buffer[65537]; gsize length;
            if (input && g_input_stream_read_all(G_INPUT_STREAM(input), buffer, sizeof buffer, &length, NULL, &error) &&
                length <= 65536 && !memchr(buffer, 0, length) && g_utf8_validate(buffer, (gssize)length, NULL)) {
                gtk_text_buffer_set_text(gtk_text_view_get_buffer(editor->source), buffer, (gint)length);
                g_autofree gchar *name = g_file_get_basename(file);
                gtk_drop_down_set_selected(editor->language, g_str_has_suffix(name, ".lua") ? 0 : 1);
                ok = TRUE;
            }
        }
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(editor->output), ok ? (save ? _("Plugin saved") : _("Plugin loaded; review and run"))
        : error ? error->message : _("Choose a UTF-8 plugin file up to 64 KiB"), -1);
}
static void choose(GtkButton *button, gpointer data)
{
    Editor *editor = data;
    FileRequest *request = g_new0(FileRequest, 1); g_weak_ref_init(&request->window, editor->window);
    request->save = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "save"));
    request->text = request->save ? source_text(editor) : NULL;
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    if (request->save) {
        gtk_file_dialog_set_initial_name(dialog, gtk_drop_down_get_selected(editor->language) ? "board.js" : "board.lua");
        gtk_file_dialog_save(dialog, GTK_WINDOW(editor->window), NULL, file_selected, request);
    } else gtk_file_dialog_open(dialog, GTK_WINDOW(editor->window), NULL, file_selected, request);
}
static GtkTextView *text_area(GtkWidget *box, const char *text, gboolean editable)
{
    GtkTextView *view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_monospace(view, TRUE); gtk_text_view_set_editable(view, editable);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view), text, -1);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_size_request(scroll, -1, 100);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(view));
    gtk_box_append(GTK_BOX(box), scroll); return view;
}
GtkWidget *tio_plugin_window(GtkWindow *parent, const char *input)
{
    Editor *editor = g_new0(Editor, 1); editor->input = g_strdup(input);
    editor->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(editor->window), _("Analysis plugin"));
    gtk_window_set_transient_for(GTK_WINDOW(editor->window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(editor->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(editor->window), 720, 640);
    g_object_set_data_full(G_OBJECT(editor->window), "plugin-editor", editor, editor_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(editor->window), box);
    GtkWidget *hint = gtk_label_new(_("API 1: define transform(input). Lua returns text; JavaScript returns text or a JSON value. Runs on this entry snapshot with a 2-second deadline and 64 KiB output limit."));
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE); gtk_box_append(GTK_BOX(box), hint);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *languages[] = {"Lua 5.4", "JavaScript", NULL};
    editor->language = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(languages));
    gtk_drop_down_set_selected(editor->language, 1); gtk_box_append(GTK_BOX(row), GTK_WIDGET(editor->language));
    const char *labels[] = {_("Load plugin…"), _("Save plugin…")};
    for (guint i = 0; i < 2; ++i) {
        GtkWidget *button = gtk_button_new_with_label(labels[i]);
        g_object_set_data(G_OBJECT(button), "save", GINT_TO_POINTER(i));
        g_signal_connect(button, "clicked", G_CALLBACK(choose), editor); gtk_box_append(GTK_BOX(row), button);
    }
    editor->run = gtk_button_new_with_label(_("Run on entry"));
    g_signal_connect(editor->run, "clicked", G_CALLBACK(run), editor); gtk_box_append(GTK_BOX(row), editor->run);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), gtk_label_new(_("Input snapshot"))); text_area(box, input, FALSE);
    gtk_box_append(GTK_BOX(box), gtk_label_new(_("Plugin source")));
    editor->source = text_area(box, "function transform(input) {\n  return {text: input, length: input.length};\n}\n", TRUE);
    gtk_box_append(GTK_BOX(box), gtk_label_new(_("Result"))); editor->output = text_area(box, "", FALSE);
    gtk_window_present(GTK_WINDOW(editor->window)); return editor->window;
}
