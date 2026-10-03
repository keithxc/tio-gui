/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* A fixed sleep does not establish that AppKit has presented a newly mapped
 * window, especially on a hosted runner. Do not hide it or start a timed peer
 * exchange while its first draw is still pending. Keep GTK criticals fatal. */
static void wait_window_drawn(GtkWindow *window)
{
    GtkWidget *widget = GTK_WIDGET(window);
    GdkFrameClock *clock = gtk_widget_get_frame_clock(widget);
    g_assert_nonnull(clock);
    const gint64 previous = gdk_frame_clock_get_frame_counter(clock);
    const gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    gtk_widget_queue_draw(widget);
    while (g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE);
        GdkFrameTimings *timings = gdk_frame_clock_get_current_timings(clock);
        if (gtk_widget_get_mapped(widget) && timings &&
            gdk_frame_timings_get_frame_counter(timings) > previous &&
            gdk_frame_timings_get_complete(timings))
            return;
        g_usleep(1000);
    }
    g_error("Window did not present a complete frame: %s", gtk_window_get_title(window));
}
