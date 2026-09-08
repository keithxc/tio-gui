/* SPDX-License-Identifier: GPL-3.0-only */
#define main tio_gui_application_main
#include "../src/main.c"
#undef main

static void settle(void)
{
    gint64 until = g_get_monotonic_time() + 200000;
    while (g_get_monotonic_time() < until) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
}

typedef struct {
    GtkAdjustment *adjustment;
    guint samples, reversals;
    double previous;
} ScrollMotion;

static void sample_scroll_frame(GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    ScrollMotion *motion = data;
    double current = gtk_adjustment_get_value(motion->adjustment);
    if (motion->samples && current < motion->previous - 0.5) ++motion->reversals;
    motion->previous = current;
    ++motion->samples;
}

/* Final position checks miss a transient backward jump between input chunks. */
static void check_fragmented_follow(TioTab *tab, GtkWidget *window)
{
    ScrollMotion motion = {.adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->highlight_view))};
    GdkFrameClock *clock = gtk_widget_get_frame_clock(window);
    gulong handler = g_signal_connect(clock, "after-paint", G_CALLBACK(sample_scroll_frame), &motion);
    const char *line = "[17:00:00.123] INFO status=ready temp=24.0 voltage=3.3\r\n";
    for (guint repeat = 0; repeat < 20; ++repeat) {
        for (gsize offset = 0; offset < strlen(line); offset += 3) {
            gsize length = MIN((gsize)3, strlen(line) - offset);
            tio_highlighter_feed(tab->highlighter, (const guint8 *)line + offset, length);
            scroll_highlight_to_bottom(tab);
            gint64 until = g_get_monotonic_time() + 8000;
            do { g_main_context_iteration(NULL, FALSE); g_usleep(500); } while (g_get_monotonic_time() < until);
        }
    }
    settle();
    g_signal_handler_disconnect(clock, handler);
    g_print("Fragmented follow: %u painted frames, %u backward jumps\n", motion.samples, motion.reversals);
    g_assert_cmpuint(motion.samples, >, 20);
    g_assert_cmpuint(motion.reversals, ==, 0);
}

int main(void)
{
    gtk_init();
    TioTab tab = {0};
    tab.highlight_follow = TRUE;
    tab.highlight_toggle = GTK_CHECK_BUTTON(gtk_check_button_new());
    g_object_ref_sink(tab.highlight_toggle);
    gtk_check_button_set_active(tab.highlight_toggle, TRUE);
    tab.scroll_bottom_button = GTK_BUTTON(gtk_button_new());
    g_object_ref_sink(tab.scroll_bottom_button);
    tab.highlight_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(tab.highlight_view, FALSE);
    gtk_text_view_set_monospace(tab.highlight_view, TRUE);
    gtk_text_view_set_wrap_mode(tab.highlight_view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(tab.highlight_view, 6);
    gtk_text_view_set_bottom_margin(tab.highlight_view, 48);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(tab.highlight_view);
    tab.highlighter = tio_highlighter_new(buffer);
    GtkWidget *window = gtk_window_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(tab.highlight_view));
    gtk_window_set_child(GTK_WINDOW(window), scroll);
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 300);
    GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab.highlight_view));
    g_signal_connect(adjustment, "value-changed", G_CALLBACK(on_highlight_scrolled), &tab);
    g_signal_connect(adjustment, "changed", G_CALLBACK(on_highlight_content_changed), &tab);
    g_signal_connect(buffer, "notify::has-selection", G_CALLBACK(on_highlight_selection_changed), &tab);
    gtk_window_present(GTK_WINDOW(window));
    for (int i = 0; i < 200; ++i) {
        tio_highlighter_feed(tab.highlighter, (const guint8 *)"INFO log line\n", 14);
    }
    settle();
    g_assert_true(tab.highlight_follow);
    g_assert_cmpfloat(gtk_adjustment_get_value(adjustment), >, 0);
    /* Interactive prompts grow as more bytes arrive;
       wrapped bursts force GtkTextView to validate its layout over frames. */
    for (int i = 0; i < 20; ++i) {
        const char *burst = "root@board:~# df -h\nFilesystem Size Used Available Use% Mounted on\n"
                            "/dev/root 128M 64M 64M 50% /\n"
                            "tmpfs 512M 0 512M 0% /tmp\nroot@board:~# ";
        tio_highlighter_feed(tab.highlighter, (const guint8 *)burst, strlen(burst));
        settle();
        g_assert_true(tab.highlight_follow);
        g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment),
            gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment), 0.5);
    }
    check_fragmented_follow(&tab, window);
    on_highlight_wheel(NULL, 0, -1, &tab);
    gtk_adjustment_set_value(adjustment, 100);
    g_assert_false(tab.highlight_follow);
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_offset(buffer, &start, 85);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, 90);
    gtk_text_buffer_select_range(buffer, &start, &end);
    g_assert_true(gtk_text_buffer_get_has_selection(buffer));
    settle();
    g_assert_true(gtk_text_buffer_get_has_selection(buffer));
    double held = gtk_adjustment_get_value(adjustment);
    for (int i = 0; i < 50; ++i) {
        tio_highlighter_feed(tab.highlighter, (const guint8 *)"WARN new line\n", 14);
    }
    settle();
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment), held, 0.5);
    g_assert_true(gtk_text_buffer_get_selection_bounds(buffer, &start, &end));
    g_assert_cmpint(gtk_text_iter_get_offset(&start), ==, 85);
    g_assert_cmpint(gtk_text_iter_get_offset(&end), ==, 90);
    gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment) -
                                        gtk_adjustment_get_page_size(adjustment));
    settle();
    g_assert_true(tab.highlight_follow);
    g_assert_false(gtk_text_buffer_get_has_selection(buffer));
    tio_highlighter_feed(tab.highlighter, (const guint8 *)"INFO more\n", 10);
    settle();
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment),
        gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment), 0.5);
    on_highlight_wheel(NULL, 0, -1, &tab);
    gtk_adjustment_set_value(adjustment, 100);
    g_assert_false(tab.highlight_follow);
    g_assert_false(on_log_return_pressed(NULL, GDK_KEY_Return, 0, 0, &tab));
    settle();
    g_assert_true(tab.highlight_follow);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment),
        gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment), 0.5);
    tio_highlighter_clear(tab.highlighter);
    queue_highlight(&tab, (const guint8 *)"prompt", 6);
    g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 0);
    settle();
    g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 6);
    g_assert_cmpuint(tab.highlight_flush_timer, ==, 0);
    queue_highlight(&tab, (const guint8 *)"discard", 7);
    reset_highlight_queue(&tab);
    tio_highlighter_clear(tab.highlighter);
    settle();
    g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 0);
    g_autofree char *large = g_strnfill(70000, 'x');
    queue_highlight(&tab, (const guint8 *)large, 70000);
    g_assert_cmpuint(tab.highlight_queue->len, <=, 65536);
    settle();
    g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 16384);
    reset_highlight_queue(&tab);
    gtk_window_destroy(GTK_WINDOW(window));
    tio_highlighter_free(tab.highlighter);
    g_object_unref(tab.highlight_toggle);
    g_object_unref(tab.scroll_bottom_button);
    g_print("Follow, pause, selection retention and resume passed.\n");
    return 0;
}
