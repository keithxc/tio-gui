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
        /* Presentation can finish after a newer frame starts, and a startup
         * frame may be skipped entirely. Look for any completed frame after
         * queue_draw, not only the newest or the first frame. */
        const gint64 first = MAX(previous + 1, gdk_frame_clock_get_history_start(clock));
        const gint64 current = gdk_frame_clock_get_frame_counter(clock);
        for (gint64 frame = first; gtk_widget_get_mapped(widget) && frame <= current; ++frame) {
            GdkFrameTimings *timings = gdk_frame_clock_get_timings(clock, frame);
            if (timings && gdk_frame_timings_get_complete(timings)) return;
        }
        g_usleep(1000);
    }
    GdkFrameTimings *timings = gdk_frame_clock_get_current_timings(clock);
    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
    g_error("Window did not present a complete frame: %s (mapped=%d, frame=%" G_GINT64_FORMAT
            ", previous=%" G_GINT64_FORMAT ", history-start=%" G_GINT64_FORMAT ", complete=%d, renderer=%s)",
            gtk_window_get_title(window), gtk_widget_get_mapped(widget),
            gdk_frame_clock_get_frame_counter(clock), previous,
            gdk_frame_clock_get_history_start(clock),
            timings && gdk_frame_timings_get_complete(timings),
            renderer ? G_OBJECT_TYPE_NAME(renderer) : "none");
}
