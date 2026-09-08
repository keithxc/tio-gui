/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ble.c"
#define ADAPTER "/org/bluez/hci0"
#define DEVICE ADAPTER "/dev_00_11_22_33_44_55"
#define CHARACTERISTIC DEVICE "/service0001/char0001"
typedef struct {
  GDBusConnection *connection;
  guint connects, disconnects, starts, stops, scans, scan_stops, writes;
  gboolean connected;
} Fake;
static const guint8 value_bytes[] = {0, 0x14, 0xff};
static const char xml[] =
    "<node>"
    "<interface name='org.freedesktop.DBus.ObjectManager'><method "
    "name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' "
    "direction='out'/></method><signal name='InterfacesAdded'><arg "
    "type='o'/><arg type='a{sa{sv}}'/></signal><signal "
    "name='InterfacesRemoved'><arg type='o'/><arg "
    "type='as'/></signal></interface>"
    "<interface name='org.bluez.Adapter1'><method "
    "name='SetDiscoveryFilter'><arg type='a{sv}' "
    "direction='in'/></method><method name='StartDiscovery'/><method "
    "name='StopDiscovery'/><property name='Powered' type='b' "
    "access='read'/></interface>"
    "<interface name='org.bluez.Device1'><method name='Connect'/><method "
    "name='Disconnect'/><property name='Alias' type='s' "
    "access='read'/><property name='Address' type='s' access='read'/><property "
    "name='Connected' type='b' access='read'/><property "
    "name='ServicesResolved' type='b' access='read'/></interface>"
    "<interface name='org.bluez.GattCharacteristic1'><method "
    "name='ReadValue'><arg type='a{sv}' direction='in'/><arg type='ay' "
    "direction='out'/></method><method name='WriteValue'><arg type='ay' "
    "direction='in'/><arg type='a{sv}' direction='in'/></method><method "
    "name='StartNotify'/><method name='StopNotify'/><property name='UUID' "
    "type='s' access='read'/><property name='Flags' type='as' "
    "access='read'/><property name='Value' type='ay' "
    "access='read'/></interface></node>";
static GVariant *props(const char *interface, Fake *fake) {
  GVariantBuilder properties;
  g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
  if (g_str_equal(interface, "org.bluez.Adapter1"))
    g_variant_builder_add(&properties, "{sv}", "Powered",
                          g_variant_new_boolean(TRUE));
  else if (g_str_equal(interface, "org.bluez.Device1")) {
    g_variant_builder_add(&properties, "{sv}", "Alias",
                          g_variant_new_string("Test board"));
    g_variant_builder_add(&properties, "{sv}", "Address",
                          g_variant_new_string("00:11:22:33:44:55"));
    g_variant_builder_add(&properties, "{sv}", "Connected",
                          g_variant_new_boolean(fake->connected));
    g_variant_builder_add(&properties, "{sv}", "ServicesResolved",
                          g_variant_new_boolean(fake->connected));
  } else {
    const char *flags[] = {"read", "write", "write-without-response", "notify",
                           NULL};
    g_variant_builder_add(
        &properties, "{sv}", "UUID",
        g_variant_new_string("0000ffe1-0000-1000-8000-00805f9b34fb"));
    g_variant_builder_add(&properties, "{sv}", "Flags",
                          g_variant_new_strv(flags, -1));
    g_variant_builder_add(&properties, "{sv}", "Value",
                          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                    value_bytes,
                                                    sizeof value_bytes, 1));
  }
  return g_variant_builder_end(&properties);
}
static void changed(Fake *fake, const char *path, const char *interface) {
  g_dbus_connection_emit_signal(
      fake->connection, NULL, path, "org.freedesktop.DBus.Properties",
      "PropertiesChanged",
      g_variant_new("(s@a{sv}as)", interface, props(interface, fake), NULL),
      NULL);
}
static void method(GDBusConnection *connection, const gchar *sender,
                   const gchar *path, const gchar *interface, const gchar *name,
                   GVariant *parameters, GDBusMethodInvocation *invocation,
                   gpointer data) {
  (void)connection;
  (void)sender;
  (void)path;
  (void)interface;
  Fake *fake = data;
  if (g_str_equal(name, "GetManagedObjects")) {
    GVariantBuilder objects;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    const char *paths[] = {ADAPTER, DEVICE, CHARACTERISTIC},
               *interfaces[] = {"org.bluez.Adapter1", "org.bluez.Device1",
                                "org.bluez.GattCharacteristic1"};
    for (guint i = 0; i < 3; ++i) {
      GVariantBuilder entry;
      g_variant_builder_init(&entry, G_VARIANT_TYPE("a{sa{sv}}"));
      g_variant_builder_add(&entry, "{s@a{sv}}", interfaces[i],
                            props(interfaces[i], fake));
      g_variant_builder_add(&objects, "{o@a{sa{sv}}}", paths[i],
                            g_variant_builder_end(&entry));
    }
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objects)));
    return;
  }
  if (g_str_equal(name, "Connect")) {
    ++fake->connects;
    fake->connected = TRUE;
    changed(fake, DEVICE, "org.bluez.Device1");
  } else if (g_str_equal(name, "Disconnect")) {
    ++fake->disconnects;
    fake->connected = FALSE;
    changed(fake, DEVICE, "org.bluez.Device1");
  } else if (g_str_equal(name, "StartDiscovery"))
    ++fake->scans;
  else if (g_str_equal(name, "StopDiscovery"))
    ++fake->scan_stops;
  else if (g_str_equal(name, "SetDiscoveryFilter")) {
    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 0);
    const char *transport = NULL;
    g_assert_true(g_variant_lookup(options, "Transport", "&s", &transport));
    g_assert_cmpstr(transport, ==, "le");
  } else if (g_str_equal(name, "ReadValue")) {
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(@ay)", g_variant_new_fixed_array(
                                               G_VARIANT_TYPE_BYTE, value_bytes,
                                               sizeof value_bytes, 1)));
    return;
  } else if (g_str_equal(name, "WriteValue")) {
    ++fake->writes;
    g_autoptr(GVariant) value = g_variant_get_child_value(parameters, 0);
    gsize length;
    const guint8 *bytes = g_variant_get_fixed_array(value, &length, 1);
    g_assert_cmpmem(bytes, length, value_bytes, sizeof value_bytes);
    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 1);
    const char *type = NULL;
    g_assert_true(g_variant_lookup(options, "type", "&s", &type));
    g_assert_cmpstr(type, ==, fake->writes == 1 ? "request" : "command");
  } else if (g_str_equal(name, "StartNotify")) {
    ++fake->starts;
    changed(fake, CHARACTERISTIC, "org.bluez.GattCharacteristic1");
  } else if (g_str_equal(name, "StopNotify"))
    ++fake->stops;
  else
    g_assert_not_reached();
  g_dbus_method_invocation_return_value(invocation, NULL);
}
static GVariant *get_property(GDBusConnection *connection, const gchar *sender,
                              const gchar *path, const gchar *interface,
                              const gchar *property, GError **error,
                              gpointer data) {
  (void)connection;
  (void)sender;
  (void)path;
  (void)error;
  g_autoptr(GVariant) properties = g_variant_ref_sink(props(interface, data));
  return g_variant_lookup_value(properties, property, NULL);
}
static const GDBusInterfaceVTable vtable = {.method_call = method,
                                            .get_property = get_property};
static void pump(guint ms) {
  gint64 until = g_get_monotonic_time() + ms * 1000;
  while (g_get_monotonic_time() < until) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
}
static void operate(BleView *view, const char *method_name) {
  GtkWidget *widget = g_object_ref_sink(gtk_button_new());
  g_object_set_data(G_OBJECT(widget), "method", (gpointer)method_name);
  action(GTK_BUTTON(widget), view);
  g_object_unref(widget);
  gint64 until = g_get_monotonic_time() + 3000000;
  while (view->busy && g_get_monotonic_time() < until)
    pump(1);
  g_assert_false(view->busy);
}
static void acceptance(void) {
  g_autoptr(GSubprocess) bus =
      g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL, "dbus-daemon",
                       "--session", "--nofork", "--print-address=1", NULL);
  g_assert_nonnull(bus);
  g_autoptr(GDataInputStream) bus_output =
      g_data_input_stream_new(g_subprocess_get_stdout_pipe(bus));
  g_autofree gchar *address =
      g_data_input_stream_read_line(bus_output, NULL, NULL, NULL);
  g_assert_nonnull(address);
  g_autoptr(GError) error = NULL;
  GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;
  GDBusConnection *service = g_dbus_connection_new_for_address_sync(
      address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GVariant) owned = g_dbus_connection_call_sync(
      service, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.bluez", 0u), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(owned);
  Fake fake = {.connection = service};
  g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, &error);
  g_assert_no_error(error);
  const char *paths[] = {"/", ADAPTER, DEVICE, CHARACTERISTIC};
  guint registrations[4];
  for (guint i = 0; i < 4; ++i) {
    registrations[i] = g_dbus_connection_register_object(
        service, paths[i], info->interfaces[i], &vtable, &fake, NULL, &error);
    g_assert_no_error(error);
  }
  GDBusConnection *client = g_dbus_connection_new_for_address_sync(
      address, flags, NULL, NULL, &error);
  g_assert_no_error(error);
  GtkWidget *parent = gtk_window_new(),
            *window = tio_ble_window_for_connection(GTK_WINDOW(parent), client);
  BleView *view = g_object_get_data(G_OBJECT(window), "ble-view");
  gint64 until = g_get_monotonic_time() + 3000000;
  while (!view->device_paths->len && g_get_monotonic_time() < until)
    pump(1);
  g_assert_cmpuint(view->device_paths->len, ==, 1);
  g_assert_cmpuint(view->characteristic_paths->len, ==, 1);
  operate(view, "StartDiscovery");
  g_assert_cmpuint(fake.scans, ==, 1);
  operate(view, "Connect");
  g_assert_cmpuint(fake.connects, ==, 1);
  operate(view, "ReadValue");
  g_assert_cmpuint(view->received, ==, 1);
  operate(view, "StartNotify");
  g_assert_cmpuint(view->received, ==, 2);
  gtk_drop_down_set_selected(view->mode, 1);
  gtk_editable_set_text(GTK_EDITABLE(view->payload), "00 14 FF");
  operate(view, "WriteValue");
  gtk_check_button_set_active(view->command, TRUE);
  operate(view, "WriteValue");
  g_assert_cmpuint(fake.writes, ==, 2);
  gtk_editable_set_text(GTK_EDITABLE(view->payload), "0");
  operate(view, "WriteValue");
  g_assert_cmpuint(fake.writes, ==, 2);
  analyze(NULL, view);
  g_assert_nonnull(view->analyzer);
  gtk_window_destroy(GTK_WINDOW(window));
  until = g_get_monotonic_time() + 3000000;
  while ((!fake.stops || !fake.scan_stops || !fake.disconnects) &&
         g_get_monotonic_time() < until)
    pump(1);
  g_assert_cmpuint(fake.stops, ==, 1);
  g_assert_cmpuint(fake.scan_stops, ==, 1);
  g_assert_cmpuint(fake.disconnects, ==, 1);
  /* Closing during an outstanding Connect still releases an acquired link. */
  window = tio_ble_window_for_connection(GTK_WINDOW(parent), client);
  view = g_object_get_data(G_OBJECT(window), "ble-view");
  until = g_get_monotonic_time() + 3000000;
  while (!view->device_paths->len && g_get_monotonic_time() < until)
    pump(1);
  call(view, DEVICE, "org.bluez.Device1", "Connect", NULL);
  gtk_window_destroy(GTK_WINDOW(window));
  until = g_get_monotonic_time() + 3000000;
  while (fake.disconnects < 2 && g_get_monotonic_time() < until)
    pump(1);
  g_assert_cmpuint(fake.disconnects, ==, 2);
  gtk_window_destroy(GTK_WINDOW(parent));
  for (guint i = 0; i < 4; ++i)
    g_dbus_connection_unregister_object(service, registrations[i]);
  g_dbus_connection_close_sync(client, NULL, NULL);
  g_dbus_connection_close_sync(service, NULL, NULL);
  g_object_unref(client);
  g_object_unref(service);
  g_subprocess_send_signal(bus, 15);
  g_subprocess_wait(bus, NULL, NULL);
}
static void system_readonly(void) {
  if (!g_getenv("TIO_TEST_BLUEZ_SYSTEM")) {
    g_test_skip(
        "Set TIO_TEST_BLUEZ_SYSTEM=1 for read-only system BlueZ inspection");
    return;
  }
  GtkWidget *parent = gtk_window_new(),
            *window = tio_ble_window(GTK_WINDOW(parent));
  BleView *view = g_object_get_data(G_OBJECT(window), "ble-view");
  gint64 until = g_get_monotonic_time() + 5000000;
  while (!view->manager && g_get_monotonic_time() < until)
    pump(1);
  g_assert_nonnull(view->manager);
  g_autofree gchar *owner = g_dbus_object_manager_client_get_name_owner(
      G_DBUS_OBJECT_MANAGER_CLIENT(view->manager));
  g_assert_nonnull(owner);
  g_assert_cmpuint(g_hash_table_size(view->owned_devices), ==, 0);
  g_assert_cmpuint(g_hash_table_size(view->discoveries), ==, 0);
  gtk_window_destroy(GTK_WINDOW(window));
  gtk_window_destroy(GTK_WINDOW(parent));
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  gtk_init();
  g_test_add_func("/ble/bluez-gatt-read-write-notify-cleanup", acceptance);
  g_test_add_func("/ble/system-readonly", system_readonly);
  return g_test_run();
}
