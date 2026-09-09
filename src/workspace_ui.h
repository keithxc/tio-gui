/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gtk/gtk.h>

/* The application workspace is shared by tio/VTE and native serial backends.
 * Capabilities remove actions; backends never choose a different row/order/style. */
typedef struct {
    GtkWidget *root, *toolbar, *options, *advanced, *search, *console, *send, *quick;
} TioWorkspaceUi;
typedef struct {
    GtkLabel *device_label, *baud_label;
    GtkDropDown *device, *baud;
    GtkEntry *custom_baud;
    GtkButton *refresh, *connect;
} TioConnectionUi;
typedef struct {
    GtkEntry *entry;
    GtkDropDown *ending;
    GtkButton *send;
    GtkMenuButton *history;
} TioSendUi;
typedef struct {
    GtkButton *buttons[4], *customize, *sequences, *analyze;
    GtkToggleButton *hex;
} TioQuickUi;
typedef struct {
    GtkSearchBar *bar;
    GtkSearchEntry *entry;
    GtkButton *previous, *next;
    GtkToggleButton *match_case, *regex;
    GtkLabel *feedback;
} TioSearchUi;
static const char *const tio_ui_baud_rates[] = {
    "9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600", "1500000", NULL
};
#define TIO_UI_CUSTOM_BAUD (G_N_ELEMENTS(tio_ui_baud_rates) - 1)
static inline GtkWidget *tio_ui_row(int spacing)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    gtk_widget_add_css_class(row, "compact-controls"); return row;
}
static inline void tio_ui_margins(GtkWidget *widget, int margin)
{
    gtk_widget_set_margin_start(widget, margin); gtk_widget_set_margin_end(widget, margin);
    gtk_widget_set_margin_top(widget, margin); gtk_widget_set_margin_bottom(widget, margin);
}
static inline TioWorkspaceUi tio_workspace_ui_new(void)
{
    TioWorkspaceUi ui = {0};
    ui.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); tio_ui_margins(ui.root, 8);
    ui.toolbar = tio_ui_row(6); ui.options = tio_ui_row(10);
    ui.advanced = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.search = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.console = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.send = tio_ui_row(6); ui.quick = tio_ui_row(6);
    GtkWidget *rows[] = {ui.toolbar, ui.options, ui.advanced, ui.search, ui.console, ui.send, ui.quick};
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) gtk_box_append(GTK_BOX(ui.root), rows[i]);
    /* Invisible optional slots must not introduce empty rows/gaps. */
    gtk_widget_set_visible(ui.advanced, FALSE); gtk_widget_set_visible(ui.search, FALSE);
    gtk_widget_set_vexpand(ui.console, TRUE); return ui;
}
static inline TioConnectionUi tio_connection_ui_new(GtkWidget *row)
{
    TioConnectionUi ui = {0};
    ui.device_label = GTK_LABEL(gtk_label_new(_("Device")));
    ui.device = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(ui.device), TRUE);
    ui.refresh = GTK_BUTTON(gtk_button_new_from_icon_name("view-refresh-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui.refresh), _("Refresh serial devices"));
    ui.baud_label = GTK_LABEL(gtk_label_new(_("Baud")));
    GtkStringList *rates = gtk_string_list_new(tio_ui_baud_rates);
    gtk_string_list_append(rates, _("Custom…"));
    ui.baud = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(rates), NULL));
    ui.custom_baud = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(ui.custom_baud, _("Custom baud"));
    gtk_entry_set_input_purpose(ui.custom_baud, GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_max_length(ui.custom_baud, 10); gtk_editable_set_width_chars(GTK_EDITABLE(ui.custom_baud), 10);
    gtk_widget_set_visible(GTK_WIDGET(ui.custom_baud), FALSE);
    ui.connect = GTK_BUTTON(gtk_button_new_with_label(_("Connect")));
    gtk_widget_add_css_class(GTK_WIDGET(ui.connect), "suggested-action");
    GtkWidget *widgets[] = {GTK_WIDGET(ui.device_label), GTK_WIDGET(ui.device), GTK_WIDGET(ui.refresh),
        GTK_WIDGET(ui.baud_label), GTK_WIDGET(ui.baud), GTK_WIDGET(ui.custom_baud), GTK_WIDGET(ui.connect)};
    for (guint i = 0; i < G_N_ELEMENTS(widgets); i++) gtk_box_append(GTK_BOX(row), widgets[i]);
    return ui;
}
static inline TioSendUi tio_send_ui_new(GtkWidget *row, gboolean history)
{
    TioSendUi ui = {0};
    if (history) {
        ui.history = GTK_MENU_BUTTON(gtk_menu_button_new());
        gtk_menu_button_set_icon_name(ui.history, "document-open-recent-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(ui.history), _("Send history"));
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.history));
    }
    ui.entry = GTK_ENTRY(gtk_entry_new()); gtk_entry_set_placeholder_text(ui.entry, _("Send text…"));
    gtk_widget_set_hexpand(GTK_WIDGET(ui.entry), TRUE); gtk_widget_set_sensitive(GTK_WIDGET(ui.entry), FALSE);
    const char *endings[] = {_("No line ending"), "LF (\\n)", "CR (\\r)", "CR+LF (\\r\\n)", NULL};
    ui.ending = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(endings)); gtk_drop_down_set_selected(ui.ending, 2);
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui.ending), _("Line ending"));
    ui.send = GTK_BUTTON(gtk_button_new_with_label(_("Send"))); gtk_widget_set_sensitive(GTK_WIDGET(ui.send), FALSE);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.entry)); gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.ending));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.send)); return ui;
}
static inline TioQuickUi tio_quick_ui_new(GtkWidget *row, gboolean sequences, gboolean analyzer)
{
    TioQuickUi ui = {0};
    for (guint i = 0; i < G_N_ELEMENTS(ui.buttons); i++) {
        ui.buttons[i] = GTK_BUTTON(gtk_button_new_with_label(""));
        gtk_widget_set_sensitive(GTK_WIDGET(ui.buttons[i]), FALSE); gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.buttons[i]));
    }
    ui.customize = GTK_BUTTON(gtk_button_new_with_label(_("Customize…")));
    gtk_widget_set_hexpand(GTK_WIDGET(ui.customize), TRUE); gtk_widget_set_halign(GTK_WIDGET(ui.customize), GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.customize));
    if (sequences) { ui.sequences = GTK_BUTTON(gtk_button_new_with_label(_("Sequences…"))); gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.sequences)); }
    if (analyzer) { ui.analyze = GTK_BUTTON(gtk_button_new_with_label(_("Analyze…"))); gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.analyze)); }
    ui.hex = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("HEX"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui.hex), _("Display incoming bytes as 16-byte hex rows"));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui.hex)); return ui;
}
static inline TioSearchUi tio_search_ui_new(void)
{
    TioSearchUi ui = {0}; ui.bar = GTK_SEARCH_BAR(gtk_search_bar_new());
    GtkWidget *row = tio_ui_row(6);
    ui.entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(ui.entry, _("Search terminal…"));
    gtk_widget_set_hexpand(GTK_WIDGET(ui.entry), TRUE);
    ui.previous = GTK_BUTTON(gtk_button_new_from_icon_name("go-up-symbolic"));
    ui.next = GTK_BUTTON(gtk_button_new_from_icon_name("go-down-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui.previous), _("Find previous"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui.next), _("Find next"));
    ui.match_case = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Aa"));
    ui.regex = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(".*"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui.match_case), _("Match case"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui.regex), _("Regular expression"));
    ui.feedback = GTK_LABEL(gtk_label_new("")); gtk_label_set_ellipsize(ui.feedback, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(ui.feedback, 28);
    GtkWidget *widgets[] = {GTK_WIDGET(ui.entry), GTK_WIDGET(ui.previous), GTK_WIDGET(ui.next),
        GTK_WIDGET(ui.match_case), GTK_WIDGET(ui.regex), GTK_WIDGET(ui.feedback)};
    for (guint i = 0; i < G_N_ELEMENTS(widgets); i++) gtk_box_append(GTK_BOX(row), widgets[i]);
    gtk_search_bar_set_child(ui.bar, row); gtk_search_bar_connect_entry(ui.bar, GTK_EDITABLE(ui.entry));
    gtk_search_bar_set_show_close_button(ui.bar, TRUE); return ui;
}
static inline GtkWidget *tio_ui_advanced_scroll(GtkWidget *content)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content); return scroll;
}
static inline GtkWidget *tio_ui_console_frame(GtkWidget *content)
{
    GtkWidget *frame = gtk_frame_new(NULL); gtk_widget_add_css_class(frame, "terminal-frame");
    gtk_widget_set_hexpand(frame, TRUE); gtk_widget_set_vexpand(frame, TRUE);
    gtk_frame_set_child(GTK_FRAME(frame), content); return frame;
}
static inline void tio_ui_console_text(GtkTextView *view)
{
    gtk_widget_add_css_class(GTK_WIDGET(view), "highlight-view");
    gtk_text_view_set_editable(view, FALSE); gtk_text_view_set_cursor_visible(view, FALSE);
    gtk_text_view_set_monospace(view, TRUE); gtk_text_view_set_wrap_mode(view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(view, 8); gtk_text_view_set_right_margin(view, 8);
    gtk_text_view_set_top_margin(view, 6); gtk_text_view_set_bottom_margin(view, 9);
}

typedef struct { GtkEntry *label, *payload; GtkDropDown *mode, *ending; } TioQuickFieldsUi;
static inline GtkWidget *tio_ui_quick_grid_new(gboolean checksum)
{
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(grid), 8); gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_vexpand(grid, TRUE);
    const char *titles[] = {_("Button"), _("Label"), _("Payload"), _("Mode"), _("Line ending"), _("Checksum")};
    for (guint i = 0; i < (checksum ? 6u : 5u); i++) {
        GtkWidget *label = gtk_label_new(titles[i]); gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_grid_attach(GTK_GRID(grid), label, (gint)i, 0, 1, 1);
    }
    return grid;
}
static inline TioQuickFieldsUi tio_ui_quick_fields_new(GtkGrid *grid, guint index, const char *label,
    const char *payload, guint mode, guint ending)
{
    TioQuickFieldsUi ui = {0}; g_autofree gchar *number = g_strdup_printf("%u", index + 1);
    GtkWidget *number_label = gtk_label_new(number); gtk_label_set_xalign(GTK_LABEL(number_label), 0);
    ui.label = GTK_ENTRY(gtk_entry_new()); gtk_editable_set_text(GTK_EDITABLE(ui.label), label);
    ui.payload = GTK_ENTRY(gtk_entry_new()); gtk_editable_set_text(GTK_EDITABLE(ui.payload), payload);
    gtk_widget_set_hexpand(GTK_WIDGET(ui.payload), TRUE);
    const char *modes[] = {_("Text"), "HEX", NULL}, *endings[] = {_("None"), "LF", "CR", "CRLF", NULL};
    ui.mode = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes)); ui.ending = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(endings));
    gtk_drop_down_set_selected(ui.mode, MIN(mode, 1)); gtk_drop_down_set_selected(ui.ending, MIN(ending, 3));
    GtkWidget *widgets[] = {number_label, GTK_WIDGET(ui.label), GTK_WIDGET(ui.payload), GTK_WIDGET(ui.mode), GTK_WIDGET(ui.ending)};
    for (guint i = 0; i < G_N_ELEMENTS(widgets); i++) gtk_grid_attach(grid, widgets[i], (gint)i, (gint)index * 3 + 1, 1, 1);
    return ui;
}

typedef struct {
    GtkWidget *root;
    GtkLabel *title, *appearance, *theme_label, *language_label;
    GtkDropDown *theme, *language;
} TioAppearanceUi;
static inline TioAppearanceUi tio_appearance_ui_new(const char *const *languages, guint language, guint theme)
{
    TioAppearanceUi ui = {0}; ui.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_size_request(ui.root, 360, -1); tio_ui_margins(ui.root, 12);
    ui.title = GTK_LABEL(gtk_label_new(_("Settings"))); gtk_label_set_xalign(ui.title, 0);
    gtk_widget_add_css_class(GTK_WIDGET(ui.title), "settings-title");
    ui.appearance = GTK_LABEL(gtk_label_new(_("Appearance"))); gtk_label_set_xalign(ui.appearance, 0);
    gtk_widget_add_css_class(GTK_WIDGET(ui.appearance), "settings-section-title");
    gtk_box_append(GTK_BOX(ui.root), GTK_WIDGET(ui.title)); gtk_box_append(GTK_BOX(ui.root), GTK_WIDGET(ui.appearance));
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7); gtk_widget_add_css_class(card, "settings-card");
    const char *themes[] = {_("Follow system"), _("Light"), _("Dark"), NULL};
    ui.theme = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(themes)); gtk_drop_down_set_selected(ui.theme, theme);
    ui.language = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(languages)); gtk_drop_down_set_selected(ui.language, language);
    ui.theme_label = GTK_LABEL(gtk_label_new(_("Theme"))); ui.language_label = GTK_LABEL(gtk_label_new(_("Language")));
    GtkLabel *labels[] = {ui.theme_label, ui.language_label}; GtkDropDown *choices[] = {ui.theme, ui.language};
    for (guint i = 0; i < 2; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_label_set_xalign(labels[i], 0); gtk_widget_set_hexpand(GTK_WIDGET(labels[i]), TRUE);
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(labels[i])); gtk_box_append(GTK_BOX(row), GTK_WIDGET(choices[i]));
        gtk_box_append(GTK_BOX(card), row);
    }
    gtk_box_append(GTK_BOX(ui.root), card); return ui;
}

static inline void tio_workspace_install_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        provider,
        ".compact-controls button, .compact-controls entry {"
        "  min-height: 26px;"
        "  padding-top: 2px;"
        "  padding-bottom: 2px;"
        "}"
        ".compact-controls dropdown button {"
        "  min-height: 26px;"
        "  padding-top: 2px;"
        "  padding-bottom: 2px;"
        "}"
        ".terminal-frame {"
        "  border: 1px solid alpha(@theme_fg_color, 0.22);"
        "  border-radius: 6px;"
        "  padding: 1px;"
        "  background-color: black;"
        "}"
        "window.tio-workspace.csd { border-radius: 12px; }"
        "window.tio-workspace.csd > headerbar { border-radius: 12px 12px 0 0; }"
        "window.tio-workspace.csd .workspace-content { border-radius: 0 0 12px 12px; background-color: @theme_bg_color; }"
        "window.tio-workspace.maximized, window.tio-workspace.fullscreen, window.tio-workspace.maximized > headerbar, window.tio-workspace.fullscreen > headerbar, window.tio-workspace.maximized .workspace-content, window.tio-workspace.fullscreen .workspace-content { border-radius: 0; }"
#ifdef G_OS_WIN32
        "textview.highlight-view, textview.highlight-view text { font-family: monospace, \"Noto Sans SC\"; }"
#endif
        "textview.highlight-view, textview.highlight-view text {"
        "  background-color: #0d1117;"
        "  color: #c9d1d9;"
        "}"
        ".session-status { font-size: 0.9em; }"
        ".settings-title {"
        "  font-size: 1.15em;"
        "  font-weight: 700;"
        "}"
        ".settings-section-title {"
        "  font-size: 0.78em;"
        "  font-weight: 700;"
        "  opacity: 0.65;"
        "  margin-top: 4px;"
        "}"
        ".settings-card {"
        "  background-color: alpha(@theme_fg_color, 0.055);"
        "  border: 1px solid alpha(@theme_fg_color, 0.12);"
        "  border-radius: 8px;"
        "  padding: 9px;"
        "}"
        ".settings-hint { font-size: 0.82em; opacity: 0.62; }"
        ".settings-version { font-weight: 700; }"
        ".scroll-bottom {"
        "  margin: 10px;"
        "  opacity: 0.92;"
        "}");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}
