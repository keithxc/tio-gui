/* SPDX-License-Identifier: GPL-3.0-only */
#define main tio_gui_application_main
#include "../src/main.c"
#undef main

static void settle(void)
{
    gint64 until = g_get_monotonic_time() + 500000;
    while (g_get_monotonic_time() < until) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
}

typedef struct {
    GtkAdjustment *adjustment;
    guint samples, reversals;
    double previous;
    guint moving;
    double max_step;
} ScrollMotion;

static void sample_scroll_frame(GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    ScrollMotion *motion = data;
    double current = gtk_adjustment_get_value(motion->adjustment);
    if (motion->samples && current < motion->previous - 0.5) ++motion->reversals;
    double step = current - motion->previous;
    if (motion->samples && step > 0.05) { ++motion->moving; motion->max_step = MAX(motion->max_step, step); }
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
            update_highlight_display(tab, (const guint8 *)line + offset, length);
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

static void check_smooth_line(TioTab *tab, GtkWidget *window)
{
    GtkAdjustment *a = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->highlight_view));
    double before = gtk_adjustment_get_value(a);
    ScrollMotion motion = {.adjustment = a, .samples = 1, .previous = before};
    GdkFrameClock *clock = gtk_widget_get_frame_clock(window);
    gulong handler = g_signal_connect(clock, "after-paint", G_CALLBACK(sample_scroll_frame), &motion);
    update_highlight_display(tab, (const guint8 *)"INFO smooth\n", 12);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(a), before, 0.01);
    settle();
    g_signal_handler_disconnect(clock, handler);
    double travel = gtk_adjustment_get_value(a) - before;
    g_assert_cmpfloat(travel, >, 5);
    g_assert_cmpuint(motion.moving, >=, 4);
    g_assert_cmpfloat(motion.max_step, <, travel * 0.7);
    g_assert_cmpuint(motion.reversals, ==, 0);
    g_assert_cmpuint(tab->highlight_scroll_tick, ==, 0);
    g_print("Smooth line: %.1f px, %u moving frames, largest step %.2f px\n", travel, motion.moving, motion.max_step);
    update_highlight_display(tab, (const guint8 *)"INFO pause\n", 11);
    gint64 deadline = g_get_monotonic_time() + 100000;
    while (!tab->highlight_scroll_tick && g_get_monotonic_time() < deadline)
        g_main_context_iteration(NULL, FALSE);
    g_assert_cmpuint(tab->highlight_scroll_tick, !=, 0);
    guint active_tick = tab->highlight_scroll_tick;
    update_highlight_display(tab, (const guint8 *)"INFO retarget\n", 14);
    g_assert_cmpuint(tab->highlight_scroll_tick, ==, active_tick);
    on_highlight_wheel(NULL, 0, -1, tab);
    g_assert_cmpuint(tab->highlight_scroll_tick, ==, 0);
    double paused = gtk_adjustment_get_value(a);
    settle();
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(a), paused, 0.01);
    on_scroll_bottom_clicked(NULL, tab);
    settle();
}

static void check_input_cursor(TioTab *tab, GtkWidget *window)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(tab->highlight_view);
    gtk_widget_grab_focus(GTK_WIDGET(tab->highlight_view));
    tab->highlight_cursor_on = TRUE;
    update_highlight_cursor(tab);
    g_assert_false(gtk_text_view_get_editable(tab->highlight_view));
    g_assert_true(gtk_text_view_get_cursor_visible(tab->highlight_view));
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
    g_assert_cmpint(gtk_text_iter_get_offset(&cursor), ==, 6);
    /* The real timeout changes visibility without changing any log bytes. */
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (gtk_text_view_get_cursor_visible(tab->highlight_view) &&
           g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    g_assert_false(gtk_text_view_get_cursor_visible(tab->highlight_view));
    g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 6);
    tab->highlight_cursor_on = TRUE;
    update_highlight_display(tab, (const guint8 *)"\b\b", 2);
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
    g_assert_cmpint(gtk_text_iter_get_offset(&cursor), ==, 4);
    update_highlight_display(tab, (const guint8 *)"\x1b[?25l", 6);
    g_assert_false(gtk_text_view_get_cursor_visible(tab->highlight_view));
    update_highlight_display(tab, (const guint8 *)"\x1b[?25h", 6);
    g_assert_true(gtk_text_view_get_cursor_visible(tab->highlight_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, 3);
    gtk_text_buffer_select_range(buffer, &start, &end);
    tab->highlight_cursor_on = TRUE;
    update_highlight_cursor(tab);
    g_assert_false(gtk_text_view_get_cursor_visible(tab->highlight_view));
    g_assert_true(gtk_text_buffer_get_selection_bounds(buffer, &start, &end));
    g_assert_cmpint(gtk_text_iter_get_offset(&end), ==, 3);
    gtk_text_buffer_place_cursor(buffer, &end);
    tab->highlight_follow = TRUE;
    update_highlight_cursor(tab);
    g_assert_true(gtk_text_view_get_cursor_visible(tab->highlight_view));
    gtk_window_set_focus(GTK_WINDOW(window), NULL);
    g_assert_false(gtk_text_view_get_cursor_visible(tab->highlight_view));
    gtk_widget_grab_focus(GTK_WIDGET(tab->highlight_view));
    g_assert_true(gtk_text_view_get_cursor_visible(tab->highlight_view));
    g_print("Input cursor: timed blink, remote position/visibility, focus and selection passed.\n");
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
    gtk_text_view_set_bottom_margin(tab.highlight_view, 9);
    g_signal_connect(tab.highlight_view, "unmap", G_CALLBACK(on_highlight_unmap), &tab);
    g_signal_connect(tab.highlight_view, "map", G_CALLBACK(on_highlight_map), &tab);
    g_signal_connect(tab.highlight_view, "notify::has-focus", G_CALLBACK(on_highlight_focus_changed), &tab);
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
    check_smooth_line(&tab, window);
    check_fragmented_follow(&tab, window);
    GString *flood = g_string_new(NULL);
    for (int i = 0; i < 200; ++i) g_string_append(flood, "INFO flood\n");
    update_highlight_display(&tab, (const guint8 *)flood->str, flood->len);
    g_string_free(flood, TRUE);
    settle();
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment),
        gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment), 0.5);
    g_assert_cmpuint(tab.highlight_scroll_tick, ==, 0);
    update_highlight_display(&tab, (const guint8 *)"INFO hidden\n", 12);
    gtk_widget_set_visible(GTK_WIDGET(tab.highlight_view), FALSE);
    g_assert_cmpuint(tab.highlight_scroll_tick, ==, 0);
    gtk_widget_set_visible(GTK_WIDGET(tab.highlight_view), TRUE);
    settle();
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
    update_highlight_display(&tab, (const guint8 *)"prompt", 6);
    g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 6);
    check_input_cursor(&tab, window);
    tio_highlighter_clear(tab.highlighter);
    settle();
    g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 0);
    gtk_window_destroy(GTK_WINDOW(window));
    g_assert_cmpuint(tab.highlight_cursor_timer, ==, 0);
    tio_highlighter_free(tab.highlighter);
    g_object_unref(tab.highlight_toggle);
    g_object_unref(tab.scroll_bottom_button);
    g_print("Follow, pause, selection retention and resume passed.\n");
    return 0;
}
