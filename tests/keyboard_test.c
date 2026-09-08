/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 600
#define main tio_gui_application_main
#include "../src/main.c"
#undef main
#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static int peer;
static gboolean receive(gpointer data)
{
    TioTab *tab = data;
    char bytes[256];
    ssize_t count = read(peer, bytes, sizeof bytes);
    if (count > 0) {
        g_print("RX:");
        for (ssize_t i = 0; i < count; ++i) g_print(" %02x", (unsigned char)bytes[i]);
        g_print("\n");
        tio_highlighter_feed(tab->highlighter, (const guint8 *)"INFO Keyboard bytes received\n", 29);
    }
    return G_SOURCE_CONTINUE;
}

int main(void)
{
    gtk_init();
    TioTab tab = {0};
    tab.child_pid = 1;
    tab.highlight_follow = TRUE;
    tab.highlight_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Highlight + keyboard test"));
    gtk_check_button_set_active(tab.highlight_toggle, TRUE);
    tab.scroll_bottom_button = GTK_BUTTON(gtk_button_new_with_label("Bottom"));
    tab.terminal = VTE_TERMINAL(vte_terminal_new());
    tab.highlight_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(tab.highlight_view, FALSE);
    gtk_text_view_set_cursor_visible(tab.highlight_view, FALSE);
    g_signal_connect(tab.highlight_view, "map", G_CALLBACK(on_highlight_map), &tab);
    g_signal_connect(tab.highlight_view, "unmap", G_CALLBACK(on_highlight_unmap), &tab);
    g_signal_connect(tab.highlight_view, "notify::has-focus", G_CALLBACK(on_highlight_focus_changed), &tab);
    tab.highlighter = tio_highlighter_new(gtk_text_view_get_buffer(tab.highlight_view));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(tab.highlight_view));
    tab.terminal_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_add_named(tab.terminal_stack, GTK_WIDGET(tab.terminal), "terminal");
    gtk_stack_add_named(tab.terminal_stack, scroll, "highlight");
    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "tio-gui isolated keyboard test");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab.highlight_toggle));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab.scroll_bottom_button));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab.terminal_stack));
    gtk_widget_set_vexpand(GTK_WIDGET(tab.terminal_stack), TRUE);
    gtk_window_set_child(GTK_WINDOW(window), box);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 500);
    gtk_window_present(GTK_WINDOW(window));
    /* Realize VTE before hiding it, matching a session widget in GtkStack. */
    gtk_stack_set_visible_child_name(tab.terminal_stack, "highlight");
    VtePty *pty = vte_pty_new_sync(VTE_PTY_DEFAULT, NULL, NULL);
    g_assert_nonnull(pty);
    int master = vte_pty_get_fd(pty);
    peer = open(ptsname(master), O_RDWR | O_NOCTTY | O_NONBLOCK);
    g_assert_cmpint(peer, >=, 0);
    struct termios attributes;
    tcgetattr(peer, &attributes);
    attributes.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    attributes.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    tcsetattr(peer, TCSANOW, &attributes);
    vte_terminal_set_pty(tab.terminal, pty);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_highlight_key_pressed), &tab);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_highlight_key_released), &tab);
    gtk_widget_add_controller(GTK_WIDGET(tab.highlight_view), keys);
    const char *sample = "[2026-09-08 12:00:00] INFO Ready addr=0x1234\n";
    tio_highlighter_feed(tab.highlighter, (const guint8 *)sample, strlen(sample));
    focus_log_view(&tab);
    g_timeout_add(25, receive, &tab);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
