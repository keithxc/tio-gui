/* SPDX-License-Identifier: GPL-3.0-only */
#include "ble.h"
#include "analyzer.h"
#include "log_model.h"
#include "payload.h"
#include <libintl.h>
#include <string.h>
#define _(s) gettext(s)
typedef struct {
  GtkWidget *window, *analyzer;
  GtkDropDown *devices, *characteristics, *mode;
  GtkEntry *payload;
  GtkCheckButton *command;
  GtkTextView *output;
  GtkLabel *status;
  GDBusObjectManager *manager;
  GPtrArray *device_paths, *characteristic_paths;
  GHashTable *notifications, *owned_devices, *discoveries;
  TioLogModel *model;
  guint scan_timer, received;
  gboolean refreshing, busy;
  gchar *pending_notify;
} BleView;
static const char *selected_path(GtkDropDown *dropdown, GPtrArray *paths) {
  guint index = gtk_drop_down_get_selected(dropdown);
  return index < paths->len ? g_ptr_array_index(paths, index) : NULL;
}
static gchar *property_string(GDBusProxy *proxy, const char *name) {
  g_autoptr(GVariant) value = g_dbus_proxy_get_cached_property(proxy, name);
  return value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)
             ? g_variant_dup_string(value, NULL)
             : g_strdup("");
}
static gboolean property_bool(GDBusProxy *proxy, const char *name) {
  g_autoptr(GVariant) value = g_dbus_proxy_get_cached_property(proxy, name);
  return value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
         g_variant_get_boolean(value);
}
static void append(BleView *view, const char *text) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->output);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_insert(buffer, &end, text, -1);
  gtk_text_buffer_insert(buffer, &end, "\n", 1);
  if (gtk_text_buffer_get_line_count(buffer) > 2000) {
    GtkTextIter begin, keep;
    gtk_text_buffer_get_start_iter(buffer, &begin);
    gtk_text_buffer_get_iter_at_line(buffer, &keep, 500);
    gtk_text_buffer_delete(buffer, &begin, &keep);
  }
}
static void received(BleView *view, const char *path, GVariant *value) {
  if (!g_variant_is_of_type(value, G_VARIANT_TYPE("ay")))
    return;
  gsize length;
  const guint8 *bytes = g_variant_get_fixed_array(value, &length, 1);
  if (length > 512) {
    gtk_label_set_text(view->status, _("BLE value exceeds 512 bytes"));
    return;
  }
  ++view->received;
  g_autoptr(GByteArray) array = g_byte_array_new();
  g_byte_array_append(array, bytes, (guint)length);
  g_autofree gchar *hex = tio_payload_preview(array),
                   *text = g_strdup_printf("%s\nRX %s", path, hex);
  append(view, text);
  tio_log_model_feed(view->model, bytes, length, g_get_real_time());
}
static void refresh(BleView *view) {
  if (!view->manager || view->refreshing)
    return;
  view->refreshing = TRUE;
  g_autofree gchar *selected_device =
      g_strdup(selected_path(view->devices, view->device_paths));
  g_autofree gchar *selected_characteristic = g_strdup(
      selected_path(view->characteristics, view->characteristic_paths));
  g_ptr_array_set_size(view->device_paths, 0);
  g_ptr_array_set_size(view->characteristic_paths, 0);
  GtkStringList *devices = gtk_string_list_new(NULL),
                *characteristics = gtk_string_list_new(NULL);
  GList *objects = g_dbus_object_manager_get_objects(view->manager);
  guint selection = 0;
  for (GList *item = objects; item && view->device_paths->len < 1000;
       item = item->next) {
    GDBusObject *object = item->data;
    g_autoptr(GDBusInterface) interface =
        g_dbus_object_get_interface(object, "org.bluez.Device1");
    if (!interface)
      continue;
    GDBusProxy *proxy = G_DBUS_PROXY(interface);
    g_autofree gchar *name = property_string(proxy, "Alias"),
                     *address = property_string(proxy, "Address");
    const char *path = g_dbus_object_get_object_path(object);
    if (g_strcmp0(path, selected_device) == 0)
      selection = view->device_paths->len;
    g_autofree gchar *label = g_strdup_printf(
        "%s · %s%s", *name ? name : address, address,
        property_bool(proxy, "Connected") ? " · connected" : "");
    gtk_string_list_append(devices, label);
    g_ptr_array_add(view->device_paths, g_strdup(path));
  }
  gtk_drop_down_set_model(view->devices, G_LIST_MODEL(devices));
  gtk_drop_down_set_selected(view->devices, selection);
  g_object_unref(devices);
  const char *device = selected_path(view->devices, view->device_paths);
  g_autofree gchar *prefix = device ? g_strconcat(device, "/", NULL) : NULL;
  selection = 0;
  for (GList *item = objects; item && view->characteristic_paths->len < 2000;
       item = item->next) {
    GDBusObject *object = item->data;
    const char *path = g_dbus_object_get_object_path(object);
    if (!prefix || !g_str_has_prefix(path, prefix))
      continue;
    g_autoptr(GDBusInterface) interface =
        g_dbus_object_get_interface(object, "org.bluez.GattCharacteristic1");
    if (!interface)
      continue;
    GDBusProxy *proxy = G_DBUS_PROXY(interface);
    g_autofree gchar *uuid = property_string(proxy, "UUID");
    g_autoptr(GVariant) flags =
        g_dbus_proxy_get_cached_property(proxy, "Flags");
    g_autofree gchar *flag_text =
        flags ? g_variant_print(flags, FALSE) : g_strdup("");
    g_autofree gchar *label = g_strdup_printf("%s %s", uuid, flag_text);
    if (g_strcmp0(path, selected_characteristic) == 0)
      selection = view->characteristic_paths->len;
    gtk_string_list_append(characteristics, label);
    g_ptr_array_add(view->characteristic_paths, g_strdup(path));
  }
  gtk_drop_down_set_model(view->characteristics, G_LIST_MODEL(characteristics));
  gtk_drop_down_set_selected(view->characteristics, selection);
  g_object_unref(characteristics);
  g_list_free_full(objects, g_object_unref);
  view->refreshing = FALSE;
}
static void changed_selection(GObject *object, GParamSpec *spec,
                              gpointer data) {
  (void)object;
  (void)spec;
  refresh(data);
}
static void objects_changed(GDBusObjectManager *manager, GDBusObject *object,
                            gpointer data) {
  (void)manager;
  (void)object;
  refresh(data);
}
static void interface_changed(GDBusObjectManager *manager, GDBusObject *object,
                              GDBusInterface *interface, gpointer data) {
  (void)manager;
  (void)object;
  (void)interface;
  refresh(data);
}
static void properties_changed(GDBusObjectManagerClient *manager,
                               GDBusObjectProxy *object, GDBusProxy *proxy,
                               GVariant *changed,
                               const gchar *const *invalidated, gpointer data) {
  (void)manager;
  (void)object;
  (void)invalidated;
  BleView *view = data;
  const char *path = g_dbus_proxy_get_object_path(proxy);
  if (g_str_equal(g_dbus_proxy_get_interface_name(proxy),
                  "org.bluez.GattCharacteristic1")) {
    g_autoptr(GVariant) value =
        g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
    if (value && (g_hash_table_contains(view->notifications, path) ||
                  g_strcmp0(view->pending_notify, path) == 0))
      received(view, path, value);
  } else
    refresh(view);
}
static void release_paths(BleView *view, GHashTable *paths,
                          const char *interface, const char *method) {
  if (!view->manager)
    return;
  GDBusConnection *connection = g_dbus_object_manager_client_get_connection(
      G_DBUS_OBJECT_MANAGER_CLIENT(view->manager));
  GHashTableIter iter;
  gpointer path;
  g_hash_table_iter_init(&iter, paths);
  while (g_hash_table_iter_next(&iter, &path, NULL))
    g_dbus_connection_call(connection, "org.bluez", path, interface, method,
                           NULL, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000,
                           NULL, NULL, NULL);
  g_hash_table_remove_all(paths);
}
static gboolean stop_scan(gpointer data) {
  BleView *view = data;
  view->scan_timer = 0;
  release_paths(view, view->discoveries, "org.bluez.Adapter1", "StopDiscovery");
  gtk_label_set_text(view->status, _("BLE scan finished"));
  return G_SOURCE_REMOVE;
}
typedef struct {
  GWeakRef window;
  gchar *path, *method;
  gboolean owned;
} Request;
static void call(BleView *view, const char *path, const char *interface,
                 const char *method, GVariant *arguments);
static void completed(GObject *source, GAsyncResult *result, gpointer data) {
  Request *request = data;
  g_autoptr(GObject) window = g_weak_ref_get(&request->window);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
  if (!window || !gtk_widget_get_visible(GTK_WIDGET(window))) {
    if (reply) {
      const char *cleanup =
          g_str_equal(request->method, "StartNotify")      ? "StopNotify"
          : g_str_equal(request->method, "StartDiscovery") ? "StopDiscovery"
          : g_str_equal(request->method, "Connect") && request->owned
              ? "Disconnect"
              : NULL;
      if (cleanup)
        g_dbus_proxy_call(G_DBUS_PROXY(source), cleanup, NULL,
                          G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
    }
  } else {
    BleView *view = g_object_get_data(window, "ble-view");
    view->busy = FALSE;
    if (g_str_equal(request->method, "StartNotify"))
      g_clear_pointer(&view->pending_notify, g_free);
    if (!reply)
      gtk_label_set_text(view->status, error->message);
    else {
      if (g_str_equal(request->method, "ReadValue") &&
          g_variant_is_of_type(reply, G_VARIANT_TYPE("(ay)"))) {
        g_autoptr(GVariant) value = g_variant_get_child_value(reply, 0);
        received(view, request->path, value);
      } else if (g_str_equal(request->method, "StartNotify"))
        g_hash_table_add(view->notifications, g_strdup(request->path));
      else if (g_str_equal(request->method, "StopNotify"))
        g_hash_table_remove(view->notifications, request->path);
      else if (g_str_equal(request->method, "Connect") && request->owned)
        g_hash_table_add(view->owned_devices, g_strdup(request->path));
      else if (g_str_equal(request->method, "Disconnect"))
        g_hash_table_remove(view->owned_devices, request->path);
      else if (g_str_equal(request->method, "StartDiscovery")) {
        g_hash_table_add(view->discoveries, g_strdup(request->path));
        if (view->scan_timer)
          g_source_remove(view->scan_timer);
        view->scan_timer = g_timeout_add_seconds(10, stop_scan, view);
      }
      g_autofree gchar *message =
          g_strdup_printf("%s: %s", request->method, _("completed"));
      gtk_label_set_text(view->status, message);
      refresh(view);
      if (g_str_equal(request->method, "SetDiscoveryFilter"))
        call(view, request->path, "org.bluez.Adapter1", "StartDiscovery", NULL);
    }
  }
  g_weak_ref_clear(&request->window);
  g_free(request->path);
  g_free(request->method);
  g_free(request);
}
static void call(BleView *view, const char *path, const char *interface,
                 const char *method, GVariant *arguments) {
  g_autoptr(GVariant) parameters =
      arguments ? g_variant_ref_sink(arguments) : NULL;
  if (view->busy) {
    gtk_label_set_text(view->status, _("Wait for the current BLE operation"));
    return;
  }
  if (path && g_str_equal(method, "StartNotify") &&
      g_hash_table_contains(view->notifications, path)) {
    gtk_label_set_text(view->status, _("Notifications already active"));
    return;
  }
  if (!view->manager || !path) {
    gtk_label_set_text(view->status,
                       _("Select an available BLE device or characteristic"));
    return;
  }
  g_autoptr(GDBusInterface) object =
      g_dbus_object_manager_get_interface(view->manager, path, interface);
  if (!object) {
    gtk_label_set_text(view->status, _("BLE object is no longer available"));
    return;
  }
  Request *request = g_new0(Request, 1);
  g_weak_ref_init(&request->window, view->window);
  request->path = g_strdup(path);
  request->method = g_strdup(method);
  request->owned = g_str_equal(method, "Connect") &&
                   !property_bool(G_DBUS_PROXY(object), "Connected");
  view->busy = TRUE;
  if (g_str_equal(method, "StartNotify"))
    view->pending_notify = g_strdup(path);
  g_dbus_proxy_call(G_DBUS_PROXY(object), method, parameters,
                    G_DBUS_CALL_FLAGS_NONE, 10000, NULL, completed, request);
}
static void action(GtkButton *button, gpointer data) {
  BleView *view = data;
  const char *method = g_object_get_data(G_OBJECT(button), "method");
  if (g_str_equal(method, "Refresh")) {
    refresh(view);
    return;
  }
  if (g_str_equal(method, "StartDiscovery")) {
    if (!view->manager)
      return;
    if (g_hash_table_size(view->discoveries)) {
      gtk_label_set_text(view->status, _("BLE scan is already running"));
      return;
    }
    GList *objects = g_dbus_object_manager_get_objects(view->manager);
    for (GList *item = objects; item; item = item->next) {
      g_autoptr(GDBusInterface) interface =
          g_dbus_object_get_interface(item->data, "org.bluez.Adapter1");
      if (interface && property_bool(G_DBUS_PROXY(interface), "Powered")) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "Transport",
                              g_variant_new_string("le"));
        call(view, g_dbus_object_get_object_path(item->data),
             "org.bluez.Adapter1", "SetDiscoveryFilter",
             g_variant_new("(a{sv})", &options));
        g_list_free_full(objects, g_object_unref);
        return;
      }
    }
    g_list_free_full(objects, g_object_unref);
    gtk_label_set_text(
        view->status,
        _("No powered Bluetooth adapter; enable it in system settings"));
    return;
  }
  gboolean device =
      g_str_equal(method, "Connect") || g_str_equal(method, "Disconnect");
  const char *path =
      selected_path(device ? view->devices : view->characteristics,
                    device ? view->device_paths : view->characteristic_paths);
  if (g_str_equal(method, "WriteValue")) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) bytes = tio_payload_build(
        gtk_editable_get_text(GTK_EDITABLE(view->payload)),
        gtk_drop_down_get_selected(view->mode) == 1, 0, 0, &error);
    if (!bytes || bytes->len > 512) {
      gtk_label_set_text(view->status,
                         error ? error->message
                               : _("BLE writes are limited to 512 bytes; the "
                                   "device may accept less"));
      return;
    }
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(
        &options, "{sv}", "type",
        g_variant_new_string(gtk_check_button_get_active(view->command)
                                 ? "command"
                                 : "request"));
    call(view, path, "org.bluez.GattCharacteristic1", method,
         g_variant_new("(@ay@a{sv})",
                       g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                 bytes->data, bytes->len, 1),
                       g_variant_builder_end(&options)));
  } else
    call(view, path,
         device ? "org.bluez.Device1" : "org.bluez.GattCharacteristic1", method,
         g_str_equal(method, "ReadValue") ? g_variant_new("(a{sv})", NULL)
                                          : NULL);
}
static void manager_ready(GObject *source, GAsyncResult *result,
                          gpointer data) {
  (void)source;
  GWeakRef *weak = data;
  g_autoptr(GObject) window = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  g_autoptr(GError) error = NULL;
  GDBusObjectManager *manager =
      g_dbus_object_manager_client_new_finish(result, &error);
  if (!window || !gtk_widget_get_visible(GTK_WIDGET(window))) {
    g_clear_object(&manager);
    return;
  }
  BleView *view = g_object_get_data(window, "ble-view");
  if (!manager) {
    gtk_label_set_text(view->status, error->message);
    return;
  }
  view->manager = manager;
  g_signal_connect(manager, "object-added", G_CALLBACK(objects_changed), view);
  g_signal_connect(manager, "object-removed", G_CALLBACK(objects_changed),
                   view);
  g_signal_connect(manager, "interface-added", G_CALLBACK(interface_changed),
                   view);
  g_signal_connect(manager, "interface-removed", G_CALLBACK(interface_changed),
                   view);
  g_signal_connect(manager, "interface-proxy-properties-changed",
                   G_CALLBACK(properties_changed), view);
  refresh(view);
  g_autofree gchar *owner = g_dbus_object_manager_client_get_name_owner(
      G_DBUS_OBJECT_MANAGER_CLIENT(manager));
  gtk_label_set_text(view->status,
                     owner ? _("BlueZ ready; scan or select a known device")
                           : _("BlueZ service is unavailable"));
}
static void start_manager(BleView *view, GDBusConnection *connection) {
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, view->window);
  g_dbus_object_manager_client_new(
      connection, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
      "org.bluez", "/", NULL, NULL, NULL, NULL, manager_ready, weak);
}
static void bus_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source;
  GWeakRef *weak = data;
  g_autoptr(GObject) window = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) connection = g_bus_get_finish(result, &error);
  if (!window || !gtk_widget_get_visible(GTK_WIDGET(window)))
    return;
  BleView *view = g_object_get_data(window, "ble-view");
  if (!connection) {
    gtk_label_set_text(view->status, error->message);
    return;
  }
  start_manager(view, connection);
}
static void analyze(GtkButton *button, gpointer data) {
  (void)button;
  BleView *view = data;
  if (view->analyzer) {
    gtk_window_present(GTK_WINDOW(view->analyzer));
    return;
  }
  view->analyzer = tio_analyzer_new(GTK_WINDOW(view->window), view->model);
  g_object_add_weak_pointer(G_OBJECT(view->analyzer),
                            (gpointer *)&view->analyzer);
}
static void free_view(gpointer data) {
  BleView *view = data;
  if (view->scan_timer)
    g_source_remove(view->scan_timer);
  release_paths(view, view->notifications, "org.bluez.GattCharacteristic1",
                "StopNotify");
  release_paths(view, view->discoveries, "org.bluez.Adapter1", "StopDiscovery");
  release_paths(view, view->owned_devices, "org.bluez.Device1", "Disconnect");
  if (view->manager)
    g_signal_handlers_disconnect_by_data(view->manager, view);
  g_clear_object(&view->manager);
  if (view->analyzer)
    gtk_window_destroy(GTK_WINDOW(view->analyzer));
  tio_log_model_free(view->model);
  g_ptr_array_unref(view->device_paths);
  g_ptr_array_unref(view->characteristic_paths);
  g_hash_table_unref(view->notifications);
  g_hash_table_unref(view->owned_devices);
  g_hash_table_unref(view->discoveries);
  g_free(view->pending_notify);
  g_free(view);
}
static void button(GtkWidget *row, const char *label, const char *method,
                   BleView *view) {
  GtkWidget *widget = gtk_button_new_with_label(label);
  g_object_set_data(G_OBJECT(widget), "method", (gpointer)method);
  g_signal_connect(widget, "clicked", G_CALLBACK(action), view);
  gtk_box_append(GTK_BOX(row), widget);
}
GtkWidget *tio_ble_window_for_connection(GtkWindow *parent,
                                         GDBusConnection *connection) {
  BleView *view = g_new0(BleView, 1);
  view->model = tio_log_model_new();
  view->device_paths = g_ptr_array_new_with_free_func(g_free);
  view->characteristic_paths = g_ptr_array_new_with_free_func(g_free);
  view->notifications =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  view->owned_devices =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  view->discoveries =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  view->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(view->window), _("BLE GATT debugging"));
  gtk_window_set_transient_for(GTK_WINDOW(view->window), parent);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(view->window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(view->window), 1050, 650);
  g_object_set_data_full(G_OBJECT(view->window), "ble-view", view, free_view);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_window_set_child(GTK_WINDOW(view->window), box);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  GtkWidget *hint = gtk_label_new(
      _("Select a device, connect, then select its GATT characteristic. "
        "Pairing and adapter power are managed in system settings. Writes keep "
        "the device's characteristic boundaries."));
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_box_append(GTK_BOX(box), hint);
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  button(row, _("Scan (10 s)"), "StartDiscovery", view);
  button(row, _("Refresh"), "Refresh", view);
  GtkWidget *analyzer = gtk_button_new_with_label(_("Analyze…"));
  g_signal_connect(analyzer, "clicked", G_CALLBACK(analyze), view);
  gtk_box_append(GTK_BOX(row), analyzer);
  gtk_box_append(GTK_BOX(box), row);
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  view->devices = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
  gtk_widget_set_hexpand(GTK_WIDGET(view->devices), TRUE);
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->devices));
  button(row, _("Connect"), "Connect", view);
  button(row, _("Disconnect"), "Disconnect", view);
  gtk_box_append(GTK_BOX(box), row);
  view->characteristics = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(view->characteristics));
  g_signal_connect(view->devices, "notify::selected",
                   G_CALLBACK(changed_selection), view);
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  button(row, _("Read"), "ReadValue", view);
  button(row, _("Start notifications"), "StartNotify", view);
  button(row, _("Stop notifications"), "StopNotify", view);
  gtk_box_append(GTK_BOX(box), row);
  view->output = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(view->output, FALSE);
  gtk_text_view_set_monospace(view->output, TRUE);
  gtk_text_view_set_wrap_mode(view->output, GTK_WRAP_WORD_CHAR);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(view->output));
  gtk_box_append(GTK_BOX(box), scroll);
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  view->payload = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(view->payload), TRUE);
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->payload));
  const char *modes[] = {_("Text"), "HEX", NULL};
  view->mode = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->mode));
  view->command =
      GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Without response")));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->command));
  button(row, _("Write"), "WriteValue", view);
  gtk_box_append(GTK_BOX(box), row);
  view->status = GTK_LABEL(gtk_label_new(_("Loading BlueZ…")));
  gtk_label_set_wrap(view->status, TRUE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(view->status));
  gtk_window_present(GTK_WINDOW(view->window));
  if (connection)
    start_manager(view, connection);
  else {
    GWeakRef *weak = g_new0(GWeakRef, 1);
    g_weak_ref_init(weak, view->window);
    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, bus_ready, weak);
  }
  return view->window;
}
GtkWidget *tio_ble_window(GtkWindow *parent) {
  return tio_ble_window_for_connection(parent, NULL);
}
