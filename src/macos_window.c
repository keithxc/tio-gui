/* SPDX-License-Identifier: GPL-3.0-only */
#include "macos_window.h"
#ifdef __APPLE__
#include <gdk/macos/gdkmacos.h>
#include <objc/message.h>
#include <objc/runtime.h>

/* AppKit constants, spelled out so this file stays plain C. */
enum { NS_FULL_SIZE_CONTENT_VIEW = 1 << 15, NS_WINDOW_TITLE_HIDDEN = 1 };

static void unify_titlebar(GtkWidget *widget)
{
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (!GDK_IS_MACOS_SURFACE(surface)) return;
    id window = (id)gdk_macos_surface_get_native_window(GDK_MACOS_SURFACE(surface));
    if (!window) return;
    unsigned long mask = ((unsigned long (*)(id, SEL))objc_msgSend)(window, sel_registerName("styleMask"));
    if (!(mask & NS_FULL_SIZE_CONTENT_VIEW))
        ((void (*)(id, SEL, unsigned long))objc_msgSend)(window, sel_registerName("setStyleMask:"), mask | NS_FULL_SIZE_CONTENT_VIEW);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(window, sel_registerName("setTitlebarAppearsTransparent:"), YES);
    ((void (*)(id, SEL, long))objc_msgSend)(window, sel_registerName("setTitleVisibility:"), NS_WINDOW_TITLE_HIDDEN);
}
static void surface_mapped(GtkWidget *widget, gpointer data) { (void)data; unify_titlebar(widget); }
static void fullscreen_changed(GObject *object, GParamSpec *pspec, gpointer data) { (void)pspec; (void)data; unify_titlebar(GTK_WIDGET(object)); }
#endif

gboolean tio_macos_unified_titlebar(GtkWindow *window)
{
#ifdef __APPLE__
    g_signal_connect_after(window, "realize", G_CALLBACK(surface_mapped), NULL);
    g_signal_connect_after(window, "map", G_CALLBACK(surface_mapped), NULL);
    g_signal_connect(window, "notify::fullscreened", G_CALLBACK(fullscreen_changed), NULL);
    if (gtk_widget_get_realized(GTK_WIDGET(window))) unify_titlebar(GTK_WIDGET(window));
    return TRUE;
#else
    (void)window; return FALSE;
#endif
}
