/* SPDX-License-Identifier: GPL-3.0-only */
#include "native_terminal.h"
#include <vterm.h>
struct _TioNativeTerminal {
    VTerm *parser;
    VTermScreen *screen;
    GtkTextView *view;
    GQueue history;
    gsize history_bytes;
    GHashTable *tags;
    gboolean dirty, alternate, cursor_visible;
    VTermPos cursor;
    int rows, cols;
    TioTerminalOutput output;
    gpointer data;
};
static int damage(VTermRect rect, void *data) { (void)rect; ((TioNativeTerminal *)data)->dirty = TRUE; return 1; }
static int cursor(VTermPos pos, VTermPos old, int visible, void *data)
{
    (void)old; TioNativeTerminal *t = data; t->cursor = pos; t->cursor_visible = visible; t->dirty = TRUE; return 1;
}
static int property(VTermProp prop, VTermValue *value, void *data)
{
    TioNativeTerminal *t = data;
    if (prop == VTERM_PROP_ALTSCREEN) t->alternate = value->boolean;
    if (prop == VTERM_PROP_CURSORVISIBLE) t->cursor_visible = value->boolean;
    t->dirty = TRUE; return 1;
}
static void cell_text(GString *text, const VTermScreenCell *cell)
{
    if (cell->chars[0] == (uint32_t)-1) return;
    if (!cell->chars[0]) { g_string_append_c(text, ' '); return; }
    for (guint i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; i++)
        if (g_unichar_validate(cell->chars[i])) g_string_append_unichar(text, cell->chars[i]);
}
static int scrollback(int cols, const VTermScreenCell *cells, void *data)
{
    TioNativeTerminal *t = data; GString *line = g_string_new(NULL);
    for (int x = 0; x < cols; x++) cell_text(line, &cells[x]);
    while (line->len && line->str[line->len - 1] == ' ') g_string_truncate(line, line->len - 1);
    g_string_append_c(line, '\n'); t->history_bytes += line->len;
    g_queue_push_tail(&t->history, g_string_free(line, FALSE));
    while (t->history.length > 2000 || t->history_bytes > 512 * 1024) {
        gchar *old = g_queue_pop_head(&t->history); t->history_bytes -= strlen(old); g_free(old);
    }
    t->dirty = TRUE; return 1;
}
static int clear_history(void *data)
{
    TioNativeTerminal *t = data; g_queue_clear_full(&t->history, g_free); t->history_bytes = 0; t->dirty = TRUE; return 1;
}
static void output(const char *bytes, size_t length, void *data)
{
    TioNativeTerminal *t = data; t->output(bytes, length, t->data);
}
TioNativeTerminal *tio_terminal_new(TioTerminalOutput callback, gpointer data)
{
    TioNativeTerminal *t = g_new0(TioNativeTerminal, 1); t->output = callback; t->data = data;
    t->rows = 24; t->cols = 80; t->cursor_visible = TRUE;
    t->parser = vterm_new(t->rows, t->cols); vterm_set_utf8(t->parser, 1);
    vterm_output_set_callback(t->parser, output, t);
    t->screen = vterm_obtain_screen(t->parser); vterm_screen_enable_altscreen(t->screen, 1);
    VTermColor fg, bg; vterm_color_rgb(&fg, 201, 209, 217); vterm_color_rgb(&bg, 13, 17, 23);
    vterm_screen_set_default_colors(t->screen, &fg, &bg);
    static const VTermScreenCallbacks callbacks = {.damage=damage, .movecursor=cursor, .settermprop=property, .sb_pushline=scrollback, .sb_clear=clear_history};
    vterm_screen_set_callbacks(t->screen, &callbacks, t); vterm_screen_reset(t->screen, 1);
    t->view = GTK_TEXT_VIEW(g_object_ref_sink(gtk_text_view_new()));
    gtk_text_view_set_editable(t->view, FALSE); gtk_text_view_set_monospace(t->view, TRUE);
    gtk_text_view_set_wrap_mode(t->view, GTK_WRAP_NONE); gtk_text_view_set_cursor_visible(t->view, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(t->view), "highlight-view");
    t->tags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return t;
}
GtkTextView *tio_terminal_view(TioNativeTerminal *t) { return t->view; }
void tio_terminal_feed(TioNativeTerminal *t, const guint8 *bytes, gsize length)
{
    vterm_input_write(t->parser, (const char *)bytes, length); vterm_screen_flush_damage(t->screen);
}
static GtkTextTag *cell_tag(TioNativeTerminal *t, VTermScreenCell *cell, gboolean cursor_)
{
    vterm_screen_convert_color_to_rgb(t->screen, &cell->fg); vterm_screen_convert_color_to_rgb(t->screen, &cell->bg);
    VTermColor fg = cell->fg, bg = cell->bg;
    if (cell->attrs.reverse != cursor_) { VTermColor swap = fg; fg = bg; bg = swap; }
    if (cell->attrs.conceal) fg = bg;
    g_autofree gchar *key = g_strdup_printf("%02x%02x%02x-%02x%02x%02x-%u%u%u", fg.rgb.red, fg.rgb.green, fg.rgb.blue,
        bg.rgb.red, bg.rgb.green, bg.rgb.blue, cell->attrs.bold, cell->attrs.italic, cell->attrs.underline);
    GtkTextTag *tag = g_hash_table_lookup(t->tags, key);
    if (!tag && g_hash_table_size(t->tags) < 1024) {
        GdkRGBA foreground = {fg.rgb.red/255.f, fg.rgb.green/255.f, fg.rgb.blue/255.f, 1};
        GdkRGBA background = {bg.rgb.red/255.f, bg.rgb.green/255.f, bg.rgb.blue/255.f, 1};
        tag = gtk_text_buffer_create_tag(gtk_text_view_get_buffer(t->view), NULL,
            "foreground-rgba", &foreground, "background-rgba", &background,
            "weight", cell->attrs.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
            "style", cell->attrs.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL,
            "underline", cell->attrs.underline ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE, NULL);
        g_hash_table_insert(t->tags, g_strdup(key), tag);
    }
    return tag;
}
void tio_terminal_render(TioNativeTerminal *t, gboolean follow)
{
    if (!gtk_widget_get_mapped(GTK_WIDGET(t->view))) return;
    g_autoptr(PangoLayout) layout = gtk_widget_create_pango_layout(GTK_WIDGET(t->view), "M"); int cw, ch;
    pango_layout_get_pixel_size(layout, &cw, &ch);
    int width = gtk_widget_get_width(GTK_WIDGET(t->view));
    /* Scrolled text view height includes history; use the viewport allocation. */
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(t->view));
    int height = parent ? gtk_widget_get_height(parent) : 480;
    int cols = CLAMP(width/MAX(1,cw), 20, 240), rows = CLAMP(height/MAX(1,ch), 5, 150);
    if (cols != t->cols || rows != t->rows) { t->cols=cols; t->rows=rows; vterm_set_size(t->parser, rows, cols); t->dirty = TRUE; }
    if (!t->dirty) return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(t->view);
    /* A selection remains stable while inspecting; the parser continues running. */
    if (gtk_text_buffer_get_has_selection(buffer) && !follow) return;
    t->dirty = FALSE; gtk_text_buffer_set_text(buffer, "", 0); GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    if (!t->alternate) for (GList *l = t->history.head; l; l=l->next) gtk_text_buffer_insert(buffer, &end, l->data, -1);
    for (int y = 0; y < t->rows; y++) {
        for (int x = 0; x < t->cols; x++) {
            VTermScreenCell cell = {0}; if (!vterm_screen_get_cell(t->screen, (VTermPos){y,x}, &cell)) continue;
            if (cell.chars[0] == (uint32_t)-1) continue;
            GString *text = g_string_new(NULL); cell_text(text, &cell);
            GtkTextTag *tag = cell_tag(t, &cell, t->cursor_visible && t->cursor.row == y && t->cursor.col == x);
            if (tag) gtk_text_buffer_insert_with_tags(buffer, &end, text->str, text->len, tag, NULL);
            else gtk_text_buffer_insert(buffer, &end, text->str, text->len);
            g_string_free(text, TRUE);
        }
        if (y + 1 < t->rows) gtk_text_buffer_insert(buffer, &end, "\n", 1);
    }
    if (follow || t->alternate) gtk_text_view_scroll_to_iter(t->view, &end, 0, FALSE, 0, 1);
}
gboolean tio_terminal_key(TioNativeTerminal *t, guint key, GdkModifierType modifiers)
{
    VTermModifier mod = (modifiers&GDK_CONTROL_MASK ? VTERM_MOD_CTRL : 0) |
        (modifiers&GDK_SHIFT_MASK ? VTERM_MOD_SHIFT : 0) | (modifiers&GDK_ALT_MASK ? VTERM_MOD_ALT : 0);
    VTermKey vk = VTERM_KEY_NONE;
    switch(key) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: vk=VTERM_KEY_ENTER; break;
    case GDK_KEY_BackSpace: vk=VTERM_KEY_BACKSPACE; break;
    case GDK_KEY_Tab: vk=VTERM_KEY_TAB; break;
    case GDK_KEY_Escape: vk=VTERM_KEY_ESCAPE; break;
    case GDK_KEY_Up: vk=VTERM_KEY_UP; break; case GDK_KEY_Down: vk=VTERM_KEY_DOWN; break;
    case GDK_KEY_Left: vk=VTERM_KEY_LEFT; break; case GDK_KEY_Right: vk=VTERM_KEY_RIGHT; break;
    case GDK_KEY_Home: vk=VTERM_KEY_HOME; break; case GDK_KEY_End: vk=VTERM_KEY_END; break;
    case GDK_KEY_Insert: vk=VTERM_KEY_INS; break; case GDK_KEY_Delete: vk=VTERM_KEY_DEL; break;
    case GDK_KEY_Page_Up: vk=VTERM_KEY_PAGEUP; break; case GDK_KEY_Page_Down: vk=VTERM_KEY_PAGEDOWN; break;
    default: if (key >= GDK_KEY_F1 && key <= GDK_KEY_F12) vk=VTERM_KEY_FUNCTION(key-GDK_KEY_F1+1); break;
    }
    if (vk != VTERM_KEY_NONE) { vterm_keyboard_key(t->parser, vk, mod); return TRUE; }
    gunichar c = gdk_keyval_to_unicode(key);
    if (c && !(modifiers & (GDK_META_MASK | GDK_SUPER_MASK))) { vterm_keyboard_unichar(t->parser, c, mod); return TRUE; }
    return FALSE;
}
void tio_terminal_paste(TioNativeTerminal *t, const char *text)
{
    vterm_keyboard_start_paste(t->parser);
    t->output(text, strlen(text), t->data); vterm_keyboard_end_paste(t->parser);
}
void tio_terminal_clear(TioNativeTerminal *t) { clear_history(t); vterm_screen_reset(t->screen, 1); t->dirty=TRUE; }
void tio_terminal_free(TioNativeTerminal *t)
{
    if (!t) return;
    vterm_free(t->parser); clear_history(t); g_hash_table_unref(t->tags); g_object_unref(t->view); g_free(t);
}
