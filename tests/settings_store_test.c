/* SPDX-License-Identifier: GPL-3.0-only */
#include "settings.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static const char *test_executable;

typedef struct {
    gchar *directory;
    gchar *primary;
    gchar *backup;
} StoreFixture;

static void remove_tree(const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
        g_assert_nonnull(directory);
        const char *name;
        while ((name = g_dir_read_name(directory)) != NULL) {
            g_autofree gchar *child = g_build_filename(path, name, NULL);
            remove_tree(child);
        }
        g_clear_pointer(&directory, g_dir_close);
        g_assert_cmpint(g_rmdir(path), ==, 0);
    } else {
        g_assert_cmpint(g_remove(path), ==, 0);
    }
}

static void setup(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    g_autoptr(GError) error = NULL;
    fixture->directory = g_dir_make_tmp("tio-settings-store-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->primary = g_build_filename(fixture->directory, "config.ini", NULL);
    fixture->backup = g_strconcat(fixture->primary, ".bk", NULL);
}

static void teardown(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    remove_tree(fixture->directory);
    g_free(fixture->primary);
    g_free(fixture->backup);
    g_free(fixture->directory);
}

static gchar *read_bytes(const char *path)
{
    gchar *contents = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    return contents;
}

/* Deliberate truncations bypass the production writer and its atomic rename. */
static void write_bytes(const char *path, const char *contents, gsize length)
{
    FILE *file = g_fopen(path, "wb");
    g_assert_nonnull(file);
    g_assert_true(fwrite(contents, 1, length, file) == length);
    g_assert_cmpint(fclose(file), ==, 0);
}

static void assert_bytes(const char *path, const char *expected)
{
    g_autofree gchar *contents = read_bytes(path);
    g_assert_cmpstr(contents, ==, expected);
}

static void replace(gchar **destination, const char *value)
{
    g_free(*destination);
    *destination = g_strdup(value);
}

static void fill_session(TioSessionConfig *session, guint index)
{
    g_autofree gchar *name = g_strdup_printf("Board %u — 测试", index);
    replace(&session->tab_name, name);
    replace(&session->device, "/dev/tty-example");
    replace(&session->device_id, "/dev/serial/by-id/example-device");
    replace(&session->baud, "250000");
    replace(&session->data_bits, "7");
    replace(&session->stop_bits, "2");
    replace(&session->parity, "odd");
    replace(&session->flow, "hard");
    session->capture_part_mb = 32;
    session->capture_part_seconds = 120;
    session->capture_keep_files = 17;
    session->capture_disk_mb = 256;
    session->reconnect = FALSE;
    session->connection_notify = TRUE;
    session->connection_sound = TRUE;
    session->auto_connect = 2;
    replace(&session->exclude_devices, "tty-example-ignored");
    replace(&session->exclude_drivers, "example-driver");
    replace(&session->exclude_tids, "example-id");
    session->dtr_default = 1;
    session->rts_default = 2;
    session->line_pulse_ms = 250;
    session->rs485 = TRUE;
    replace(&session->rs485_config, "RTS_ON_SEND=1,RX_DURING_TX");
    session->local_echo = TRUE;
    session->hex_output = TRUE;
    session->timestamps = TRUE;
    replace(&session->timestamp_format, "24hour-delta");
    replace(&session->line_ending, "crlf");
    session->output_delay = 12;
    session->output_line_delay = 34;
    session->logging = TRUE;
    replace(&session->log_directory, "example-logs");
    replace(&session->log_file, "capture.log");
    session->log_append = TRUE;
    session->log_strip = FALSE;
    for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
        g_autofree gchar *label = g_strdup_printf("Command %u", i);
        replace(&session->quick_labels[i], i == 0 ? "" : label);
        replace(&session->quick_payloads[i], i == 0 ? "" : "AT\r\n;\\x00");
        session->quick_delays[i] = 17 + i;
        session->quick_modes[i] = i % 2;
        session->quick_endings[i] = i;
        session->quick_crcs[i] = i;
    }
}

static void fill_settings(TioSettings *settings)
{
    tio_settings_init(settings);
    fill_session(&settings->defaults, 0);
    replace(&settings->language, "en");
    replace(&settings->theme, "dark");
    settings->native_tools_version = 1;
    settings->font_size = 19;
    replace(&settings->active_profile, "Second board");
    settings->show_all_ttys = TRUE;
    settings->advanced_expanded = TRUE;
    settings->log_warning_mb = 512;
    settings->restore_tabs = FALSE;
    replace(&settings->highlight_rules, "ERROR;红色\nWARN;orange");
    g_ptr_array_add(settings->sequences, g_strdup("sequence-one;AT\\r;25"));
    g_ptr_array_add(settings->sequences, g_strdup("sequence-two;00 FF;50"));
    tio_settings_push_history(settings, "first;command");
    tio_settings_push_history(settings, "second\ncommand");
    for (guint i = 1; i <= 3; ++i) {
        TioSessionConfig session;
        tio_session_config_init(&session);
        fill_session(&session, i);
        session.output_delay += i;
        tio_settings_add_tab(settings, &session);
        if (i < 3)
            tio_settings_store_profile(settings, i == 1 ? "First board" : "Second board", &session);
        tio_session_config_clear(&session);
    }
}

static void assert_session(const TioSessionConfig *actual, const TioSessionConfig *expected)
{
#define STRING_FIELD(field) g_assert_cmpstr(actual->field, ==, expected->field)
#define VALUE_FIELD(field) g_assert_true(actual->field == expected->field)
    STRING_FIELD(tab_name); STRING_FIELD(device); STRING_FIELD(device_id);
    STRING_FIELD(baud); STRING_FIELD(data_bits); STRING_FIELD(stop_bits);
    STRING_FIELD(parity); STRING_FIELD(flow); STRING_FIELD(exclude_devices);
    STRING_FIELD(exclude_drivers); STRING_FIELD(exclude_tids); STRING_FIELD(rs485_config);
    STRING_FIELD(timestamp_format); STRING_FIELD(line_ending);
    STRING_FIELD(log_directory); STRING_FIELD(log_file);
    VALUE_FIELD(capture_part_mb); VALUE_FIELD(capture_part_seconds);
    VALUE_FIELD(capture_keep_files); VALUE_FIELD(capture_disk_mb);
    VALUE_FIELD(reconnect); VALUE_FIELD(connection_notify); VALUE_FIELD(connection_sound);
    VALUE_FIELD(auto_connect); VALUE_FIELD(dtr_default); VALUE_FIELD(rts_default);
    VALUE_FIELD(line_pulse_ms); VALUE_FIELD(rs485); VALUE_FIELD(local_echo);
    VALUE_FIELD(hex_output); VALUE_FIELD(timestamps); VALUE_FIELD(output_delay);
    VALUE_FIELD(output_line_delay); VALUE_FIELD(logging); VALUE_FIELD(log_append);
    VALUE_FIELD(log_strip);
    for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
        STRING_FIELD(quick_labels[i]); STRING_FIELD(quick_payloads[i]);
        VALUE_FIELD(quick_delays[i]); VALUE_FIELD(quick_modes[i]);
        VALUE_FIELD(quick_endings[i]); VALUE_FIELD(quick_crcs[i]);
    }
#undef STRING_FIELD
#undef VALUE_FIELD
}

static void assert_settings(const TioSettings *actual, const TioSettings *expected)
{
    assert_session(&actual->defaults, &expected->defaults);
    g_assert_cmpstr(actual->language, ==, expected->language);
    g_assert_cmpstr(actual->theme, ==, expected->theme);
    g_assert_cmpstr(actual->active_profile, ==, expected->active_profile);
    g_assert_cmpstr(actual->highlight_rules, ==, expected->highlight_rules);
    g_assert_cmpuint(actual->native_tools_version, ==, expected->native_tools_version);
    g_assert_cmpuint(actual->font_size, ==, expected->font_size);
    g_assert_true(actual->show_all_ttys == expected->show_all_ttys);
    g_assert_true(actual->advanced_expanded == expected->advanced_expanded);
    g_assert_cmpuint(actual->log_warning_mb, ==, expected->log_warning_mb);
    g_assert_true(actual->restore_tabs == expected->restore_tabs);
    g_assert_cmpuint(actual->history->len, ==, expected->history->len);
    g_assert_cmpuint(actual->sequences->len, ==, expected->sequences->len);
    g_assert_cmpuint(actual->profiles->len, ==, expected->profiles->len);
    g_assert_cmpuint(actual->tab_configs->len, ==, expected->tab_configs->len);
    for (guint i = 0; i < actual->history->len; ++i)
        g_assert_cmpstr(g_ptr_array_index(actual->history, i), ==, g_ptr_array_index(expected->history, i));
    for (guint i = 0; i < actual->sequences->len; ++i)
        g_assert_cmpstr(g_ptr_array_index(actual->sequences, i), ==, g_ptr_array_index(expected->sequences, i));
    for (guint i = 0; i < actual->profiles->len; ++i) {
        const TioProfile *left = g_ptr_array_index(actual->profiles, i);
        const TioProfile *right = g_ptr_array_index(expected->profiles, i);
        g_assert_cmpstr(left->name, ==, right->name);
        assert_session(&left->session, &right->session);
    }
    for (guint i = 0; i < actual->tab_configs->len; ++i)
        assert_session(g_ptr_array_index(actual->tab_configs, i), g_ptr_array_index(expected->tab_configs, i));
}

static void save_store(const TioSettings *settings, const char *path)
{
    /* Catch hidden generation/slot mutations through the const input, too. */
    TioSettings before;
    memcpy(&before, settings, sizeof before);
    g_autoptr(GError) error = NULL;
    g_assert_true(tio_settings_save_to_store(settings, path, &error));
    g_assert_no_error(error);
    g_assert_true(memcmp(&before, settings, sizeof before) == 0);
}

static void assert_loaded(const char *path, const TioSettings *expected)
{
    TioSettings actual;
    tio_settings_init(&actual);
    g_autoptr(GError) error = NULL;
    g_assert_true(tio_settings_load_from_store(&actual, path, &error));
    g_assert_no_error(error);
    assert_settings(&actual, expected);
    /* Reusing a live settings object must replace arrays, not append again. */
    g_assert_true(tio_settings_load_from_store(&actual, path, &error));
    g_assert_no_error(error);
    assert_settings(&actual, expected);
    tio_settings_clear(&actual);
}

static void assert_font(const char *path, guint expected)
{
    TioSettings actual;
    tio_settings_init(&actual);
    g_autoptr(GError) error = NULL;
    g_assert_true(tio_settings_load_from_store(&actual, path, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(actual.font_size, ==, expected);
    tio_settings_clear(&actual);
}

/* Build on-disk fixtures directly from the documented wire format, independently
 * of save_to_store. This also exercises unsigned serial-number comparisons. */
static gchar *framed(const char *payload, guint64 sequence)
{
    g_autofree gchar *number = g_strdup_printf("%" G_GUINT64_FORMAT, sequence);
    g_autofree gchar *checked = g_strconcat(number, "\n", payload, NULL);
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, checked, -1);
    return g_strdup_printf("[storage]\nversion=1\nsequence=%s\nsha256=%s\n\n%s", number, digest, payload);
}

static void write_frame(const char *path, const char *payload, guint64 sequence)
{
    g_autofree gchar *contents = framed(payload, sequence);
    write_bytes(path, contents, strlen(contents));
}

static gchar *assert_frame(const char *path, guint64 sequence)
{
    g_autofree gchar *contents = read_bytes(path);
    const char *separator = strstr(contents, "\n\n");
    g_assert_nonnull(separator);
    const char *payload = separator + 2;
    g_autofree gchar *expected = framed(payload, sequence);
    g_assert_cmpstr(contents, ==, expected);
    g_assert_null(strstr(payload, "[storage]"));
    return g_strdup(payload);
}

static void missing_and_alternation(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    TioSettings settings;
    TioSettings expected;
    fill_settings(&settings);
    fill_settings(&expected);
    g_autoptr(GError) error = NULL;
    g_assert_false(tio_settings_load_from_store(&settings, fixture->primary, &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
    assert_settings(&settings, &expected);
    for (guint64 sequence = 0; sequence < 4; ++sequence) {
        const char *target = sequence % 2 == 0 ? fixture->primary : fixture->backup;
        const char *other = sequence % 2 == 0 ? fixture->backup : fixture->primary;
        g_autofree gchar *previous = sequence ? read_bytes(other) : NULL;
        settings.font_size = expected.font_size = 19 + (guint)sequence;
        save_store(&settings, fixture->primary);
        assert_settings(&settings, &expected);
        g_autofree gchar *payload = assert_frame(target, sequence);
        if (sequence == 0)
            g_assert_false(g_file_test(fixture->backup, G_FILE_TEST_EXISTS));
        else
            assert_bytes(other, previous);
        assert_loaded(fixture->primary, &expected);
    }
    tio_settings_clear(&settings);
    tio_settings_clear(&expected);
}

static const char old_payload[] = "[general]\nfont-size=17\n[defaults]\nbaud=9600\n";
static const char new_payload[] = "[general]\nfont-size=23\n[defaults]\nbaud=250000\n";

static void corrupt_slot_recovery(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    /* Each corruption must fall back in both directions, regardless of slot. */
    for (guint latest_primary = 0; latest_primary < 2; ++latest_primary) {
        const char *old_path = latest_primary ? fixture->backup : fixture->primary;
        const char *new_path = latest_primary ? fixture->primary : fixture->backup;
        write_frame(old_path, old_payload, 8);
        g_autofree gchar *old_bytes = read_bytes(old_path);
        g_autofree gchar *valid = framed(new_payload, 9);
        const char *needles[] = { "sequence=9", "sha256=", "font-size=23", "version=1" };
        for (guint i = 0; i < G_N_ELEMENTS(needles); ++i) {
            g_autofree gchar *corrupt = g_strdup(valid);
            char *field = strstr(corrupt, needles[i]);
            g_assert_nonnull(field);
            char *value = strchr(field, '=') + 1;
            value[0] = value[0] == '0' ? '1' : '0';
            write_bytes(new_path, corrupt, strlen(corrupt));
            assert_font(fixture->primary, 17);
            assert_bytes(old_path, old_bytes);
        }
        /* A correctly checksummed but syntactically invalid INI is still bad. */
        write_frame(new_path, "[general\nfont-size=23\n", 9);
        assert_font(fixture->primary, 17);
        /* Every shorter prefix includes empty, partial headers and valid-looking
         * INI truncations. The full payload length is the first valid new slot. */
        for (gsize length = 0; length < strlen(valid); ++length) {
            write_bytes(new_path, valid, length);
            assert_font(fixture->primary, 17);
        }
        write_bytes(new_path, valid, strlen(valid));
        assert_font(fixture->primary, 23);
        assert_bytes(old_path, old_bytes);
    }
}

static void legacy_migration(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    TioSettings settings;
    fill_settings(&settings);
    g_autoptr(GError) error = NULL;
    g_assert_true(tio_settings_save_to_file(&settings, fixture->primary, &error));
    g_assert_no_error(error);
    g_autofree gchar *legacy = read_bytes(fixture->primary);
    g_assert_null(strstr(legacy, "[storage]"));
    assert_loaded(fixture->primary, &settings);
    settings.font_size = 27;
    save_store(&settings, fixture->primary);
    assert_bytes(fixture->primary, legacy);
    g_autofree gchar *payload = assert_frame(fixture->backup, 1);
    assert_loaded(fixture->primary, &settings);
    g_autofree gchar *backup = read_bytes(fixture->backup);
    settings.font_size = 28;
    save_store(&settings, fixture->primary);
    g_autofree gchar *next_payload = assert_frame(fixture->primary, 2);
    assert_bytes(fixture->backup, backup);
    assert_loaded(fixture->primary, &settings);
    tio_settings_clear(&settings);
}

static void backup_only(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    write_frame(fixture->backup, old_payload, 41);
    g_autofree gchar *backup = read_bytes(fixture->backup);
    assert_font(fixture->primary, 17);
    TioSettings settings;
    fill_settings(&settings);
    save_store(&settings, fixture->primary);
    g_autofree gchar *payload = assert_frame(fixture->primary, 42);
    assert_bytes(fixture->backup, backup);
    assert_loaded(fixture->primary, &settings);
    tio_settings_clear(&settings);
}

static void early_legacy_migration(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    const char legacy[] =
        "[serial]\ndevice=/dev/tty-example\nbaud=57600\nlocal-echo=true\n"
        "[display]\nlanguage=en\ntimestamps=true\nhex-output=true\n"
        "[logging]\nenabled=true\ndirectory=example-logs\n"
        "[quick-buttons]\nlabel-1=Probe\npayload-1=AT\\r\\n\n";
    write_bytes(fixture->primary, legacy, strlen(legacy));
    TioSettings settings;
    tio_settings_init(&settings);
    g_autoptr(GError) error = NULL;
    g_assert_true(tio_settings_load_from_store(&settings, fixture->primary, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(settings.defaults.device, ==, "/dev/tty-example");
    g_assert_cmpstr(settings.defaults.baud, ==, "57600");
    g_assert_cmpstr(settings.language, ==, "en");
    g_assert_true(settings.defaults.local_echo);
    g_assert_true(settings.defaults.timestamps);
    g_assert_true(settings.defaults.hex_output);
    g_assert_true(settings.defaults.logging);
    g_assert_cmpstr(settings.defaults.log_directory, ==, "example-logs");
    g_assert_cmpstr(settings.defaults.quick_labels[0], ==, "Probe");
    g_assert_cmpstr(settings.defaults.quick_payloads[0], ==, "AT\r\n");
    save_store(&settings, fixture->primary);
    assert_bytes(fixture->primary, legacy);
    g_autofree gchar *payload = assert_frame(fixture->backup, 1);
    assert_loaded(fixture->primary, &settings);
    tio_settings_clear(&settings);
}

static void sequence_wrap(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    write_frame(fixture->primary, old_payload, G_MAXUINT64 - 1);
    write_frame(fixture->backup, new_payload, G_MAXUINT64);
    assert_font(fixture->primary, 23);
    g_autofree gchar *backup = read_bytes(fixture->backup);
    TioSettings settings;
    fill_settings(&settings);
    save_store(&settings, fixture->primary);
    g_autofree gchar *payload_zero = assert_frame(fixture->primary, 0);
    assert_bytes(fixture->backup, backup);
    assert_loaded(fixture->primary, &settings);
    g_autofree gchar *primary = read_bytes(fixture->primary);
    settings.font_size = 29;
    save_store(&settings, fixture->primary);
    g_autofree gchar *payload_one = assert_frame(fixture->backup, 1);
    assert_bytes(fixture->primary, primary);
    assert_loaded(fixture->primary, &settings);
    /* Also cross the boundary with primary holding MAX, then backup holding 0. */
    write_frame(fixture->backup, old_payload, G_MAXUINT64 - 1);
    write_frame(fixture->primary, new_payload, G_MAXUINT64);
    assert_font(fixture->primary, 23);
    save_store(&settings, fixture->primary);
    g_autofree gchar *backup_zero = assert_frame(fixture->backup, 0);
    assert_loaded(fixture->primary, &settings);
    save_store(&settings, fixture->primary);
    g_autofree gchar *primary_one = assert_frame(fixture->primary, 1);
    assert_loaded(fixture->primary, &settings);
    tio_settings_clear(&settings);
}

static void assert_rejected_unchanged(StoreFixture *fixture)
{
    TioSettings settings;
    TioSettings expected;
    fill_settings(&settings);
    fill_settings(&expected);
    g_autofree gchar *primary = read_bytes(fixture->primary);
    g_autofree gchar *backup = read_bytes(fixture->backup);
    g_autoptr(GError) error = NULL;
    g_assert_false(tio_settings_load_from_store(&settings, fixture->primary, &error));
    g_assert_nonnull(error);
    assert_settings(&settings, &expected);
    g_clear_error(&error);
    g_assert_false(tio_settings_save_to_store(&settings, fixture->primary, &error));
    g_assert_nonnull(error);
    assert_settings(&settings, &expected);
    assert_bytes(fixture->primary, primary);
    assert_bytes(fixture->backup, backup);
    tio_settings_clear(&settings);
    tio_settings_clear(&expected);
}

static void equal_and_ambiguous_sequences(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    write_frame(fixture->primary, old_payload, 12);
    write_frame(fixture->backup, new_payload, 12);
    assert_font(fixture->primary, 17);
    g_autofree gchar *primary = read_bytes(fixture->primary);
    TioSettings settings;
    fill_settings(&settings);
    save_store(&settings, fixture->primary);
    g_autofree gchar *payload = assert_frame(fixture->backup, 13);
    assert_bytes(fixture->primary, primary);
    assert_loaded(fixture->primary, &settings);
    tio_settings_clear(&settings);
    write_frame(fixture->primary, old_payload, 0);
    write_frame(fixture->backup, new_payload, G_GUINT64_CONSTANT(1) << 63);
    assert_rejected_unchanged(fixture);
    write_frame(fixture->primary, old_payload, G_GUINT64_CONSTANT(1) << 63);
    write_frame(fixture->backup, new_payload, 0);
    assert_rejected_unchanged(fixture);
}

static void all_copies_bad(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    const char *bad[] = { "[storage]\nversion=1\nsequence=2\n", "not an ini file", "" };
    for (guint i = 0; i < G_N_ELEMENTS(bad); ++i) {
        write_bytes(fixture->primary, bad[i], strlen(bad[i]));
        write_bytes(fixture->backup, bad[i], strlen(bad[i]));
        assert_rejected_unchanged(fixture);
    }
}

static void failed_write_preserves_latest(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    for (guint latest_primary = 0; latest_primary < 2; ++latest_primary) {
        const char *latest = latest_primary ? fixture->primary : fixture->backup;
        const char *target = latest_primary ? fixture->backup : fixture->primary;
        write_frame(latest, old_payload, 4);
        g_autofree gchar *before = read_bytes(latest);
        g_assert_cmpint(g_mkdir(target, 0700), ==, 0);
        g_autofree gchar *sentinel = g_build_filename(target, "keep", NULL);
        write_bytes(sentinel, "directory must survive", strlen("directory must survive"));
        TioSettings settings;
        fill_settings(&settings);
        g_autoptr(GError) error = NULL;
        g_assert_false(tio_settings_save_to_store(&settings, fixture->primary, &error));
        g_assert_nonnull(error);
        assert_bytes(latest, before);
        assert_bytes(sentinel, "directory must survive");
        assert_font(fixture->primary, 17);
        tio_settings_clear(&settings);
        remove_tree(target);
        g_assert_cmpint(g_remove(latest), ==, 0);
    }
}

static void single_file_import_export(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    TioSettings settings;
    TioSettings expected;
    fill_settings(&settings);
    fill_settings(&expected);
    save_store(&settings, fixture->primary);
    save_store(&settings, fixture->primary);
    g_autofree gchar *primary = read_bytes(fixture->primary);
    g_autofree gchar *backup = read_bytes(fixture->backup);
    g_autofree gchar *export_path = g_build_filename(fixture->directory, "export.ini", NULL);
    g_autofree gchar *export_backup = g_strconcat(export_path, ".bk", NULL);
    g_autofree gchar *export_lock = g_strconcat(export_path, ".lock", NULL);
    g_autoptr(GError) error = NULL;
    g_assert_true(tio_settings_save_to_file(&settings, export_path, &error));
    g_assert_no_error(error);
    g_autofree gchar *plain = read_bytes(export_path);
    g_assert_null(strstr(plain, "[storage]"));
    g_autofree gchar *stored_payload = assert_frame(fixture->backup, 1);
    g_assert_cmpstr(plain, ==, stored_payload);
    TioSettings imported;
    tio_settings_init(&imported);
    g_assert_true(tio_settings_load_from_file(&imported, export_path, &error));
    g_assert_no_error(error);
    assert_settings(&imported, &settings);
    tio_settings_clear(&imported);
    g_assert_true(tio_settings_export_portable(&settings, export_path, &error));
    g_assert_no_error(error);
    g_autoptr(GKeyFile) portable = g_key_file_new();
    g_assert_true(g_key_file_load_from_file(portable, export_path, G_KEY_FILE_NONE, &error));
    g_assert_no_error(error);
    g_assert_false(g_key_file_has_group(portable, "storage"));
    g_assert_false(g_key_file_has_group(portable, "tab:0"));
    g_assert_false(g_key_file_has_key(portable, "send", "history", NULL));
    const char *groups[] = { "defaults", "profile:First board", "profile:Second board" };
    for (guint i = 0; i < G_N_ELEMENTS(groups); ++i) {
        g_assert_true(g_key_file_has_group(portable, groups[i]));
        g_assert_false(g_key_file_has_key(portable, groups[i], "device", NULL));
        g_assert_false(g_key_file_has_key(portable, groups[i], "device-id", NULL));
        g_autofree gchar *directory = g_key_file_get_string(portable, groups[i], "log-directory", NULL);
        g_assert_cmpstr(directory, ==, "");
    }
    g_assert_false(g_file_test(export_backup, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(export_lock, G_FILE_TEST_EXISTS));
    assert_bytes(fixture->primary, primary);
    assert_bytes(fixture->backup, backup);
    assert_settings(&settings, &expected);
    tio_settings_clear(&settings);
    tio_settings_clear(&expected);
}

static void written_slot_permissions(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
#ifdef G_OS_UNIX
    TioSettings settings;
    fill_settings(&settings);
    save_store(&settings, fixture->primary);
    save_store(&settings, fixture->primary);
    for (guint i = 0; i < 2; ++i) {
        const char *target = i == 0 ? fixture->primary : fixture->backup;
        g_assert_cmpint(g_chmod(target, 0644), ==, 0);
        save_store(&settings, fixture->primary);
        GStatBuf state;
        g_assert_cmpint(g_stat(target, &state), ==, 0);
        g_assert_cmpuint(state.st_mode & 0777, ==, 0600);
        assert_loaded(fixture->primary, &settings);
    }
    tio_settings_clear(&settings);
#else
    (void)fixture;
    g_test_skip("POSIX file modes are not Windows ACLs");
#endif
}

static GSubprocess *start_child(const char *mode, const char *path)
{
    g_autoptr(GError) error = NULL;
    GSubprocess *child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error,
                                         test_executable, mode, path, NULL);
    g_assert_no_error(error);
    g_assert_nonnull(child);
    g_autoptr(GDataInputStream) output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(child));
    /* The Windows CRT writes CRLF in text mode; accept either platform's
       line ending while still checking the exact readiness message. */
    g_data_input_stream_set_newline_type(output, G_DATA_STREAM_NEWLINE_TYPE_ANY);
    g_autofree gchar *ready = g_data_input_stream_read_line(output, NULL, NULL, &error);
    if (error || g_strcmp0(ready, "ready") != 0) {
        /* Do not leave a lock-holding child alive when an assertion aborts. */
        g_subprocess_force_exit(child);
        g_subprocess_wait(child, NULL, NULL);
    }
    g_assert_no_error(error);
    g_assert_cmpstr(ready, ==, "ready");
    return child;
}

static void kill_child(GSubprocess *child)
{
    g_subprocess_force_exit(child);
    g_autoptr(GError) error = NULL;
    g_assert_true(g_subprocess_wait(child, NULL, &error));
    g_assert_no_error(error);
    g_assert_false(g_subprocess_get_successful(child));
}

static void cross_process_lock(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    TioSettings settings;
    fill_settings(&settings);
    save_store(&settings, fixture->primary);
    save_store(&settings, fixture->primary);
    g_autofree gchar *primary = read_bytes(fixture->primary);
    g_autofree gchar *backup = read_bytes(fixture->backup);
    g_autoptr(GSubprocess) child = start_child("--hold-store-lock", fixture->primary);
    g_autoptr(GError) error = NULL;
    g_assert_false(tio_settings_save_to_store(&settings, fixture->primary, &error));
    g_assert_nonnull(error);
    g_assert_cmpuint(error->domain, ==, G_FILE_ERROR);
    g_assert_true(error->code == G_FILE_ERROR_AGAIN || error->code == G_FILE_ERROR_ACCES);
    assert_bytes(fixture->primary, primary);
    assert_bytes(fixture->backup, backup);
    /* Readers need no lock and must not replace a readable workspace with defaults. */
    assert_loaded(fixture->primary, &settings);
    kill_child(child);
    /* No explicit unlock or removal of a stale .lock file was performed. */
    save_store(&settings, fixture->primary);
    g_autofree gchar *payload = assert_frame(fixture->primary, 2);
    assert_bytes(fixture->backup, backup);
    assert_loaded(fixture->primary, &settings);
    tio_settings_clear(&settings);
}

static void interrupted_writer(StoreFixture *fixture, gconstpointer unused)
{
    (void)unused;
    TioSettings expected;
    fill_settings(&expected);
    save_store(&expected, fixture->primary);
    /* Kill real writers after at least one committed snapshot. Vary the delay
     * to interrupt different points of the write/rename loop. This exercises
     * process death, not a claim to reproduce hardware power-loss semantics. */
    const gulong delays[] = { 1000, 3000, 7000, 11000 };
    for (guint i = 0; i < G_N_ELEMENTS(delays); ++i) {
        g_autoptr(GSubprocess) child = start_child("--write-store-loop", fixture->primary);
        g_usleep(delays[i]);
        kill_child(child);
        TioSettings loaded;
        tio_settings_init(&loaded);
        g_autoptr(GError) error = NULL;
        g_assert_true(tio_settings_load_from_store(&loaded, fixture->primary, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(loaded.font_size, >=, 19);
        g_assert_cmpuint(loaded.font_size, <=, 38);
        expected.font_size = loaded.font_size;
        assert_settings(&loaded, &expected);
        /* The interrupted writer must also have released the write lock. */
        save_store(&loaded, fixture->primary);
        assert_loaded(fixture->primary, &loaded);
        tio_settings_clear(&loaded);
    }
    tio_settings_clear(&expected);
}

static int child_hold_lock(const char *path)
{
    g_autofree gchar *lock_path = g_strconcat(path, ".lock", NULL);
#ifdef G_OS_WIN32
    g_autofree gunichar2 *wide = g_utf8_to_utf16(lock_path, -1, NULL, NULL, NULL);
    g_assert_nonnull(wide);
    HANDLE handle = CreateFileW((LPCWSTR)wide, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_assert_true(handle != INVALID_HANDLE_VALUE);
    OVERLAPPED overlap = {0};
    g_assert_true(LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                              0, 1, 0, &overlap));
#else
    int fd = g_open(lock_path, O_RDWR | O_CREAT, 0600);
    g_assert_cmpint(fd, >=, 0);
    struct flock region = {0};
    region.l_type = F_WRLCK;
    region.l_whence = SEEK_SET;
    g_assert_cmpint(fcntl(fd, F_SETLK, &region), ==, 0);
#endif
    puts("ready");
    fflush(stdout);
    for (;;)
        g_usleep(G_USEC_PER_SEC);
    /* Parent terminates us without running cleanup, testing kernel lock release. */
}

static int child_write_loop(const char *path)
{
    TioSettings settings;
    fill_settings(&settings);
    for (guint i = 0; ; ++i) {
        settings.font_size = 19 + i % 20;
        save_store(&settings, path);
        if (i == 0) {
            puts("ready");
            fflush(stdout);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--hold-store-lock") == 0)
        return child_hold_lock(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--write-store-loop") == 0)
        return child_write_loop(argv[2]);
    g_autofree gchar *executable = g_find_program_in_path(argv[0]);
    g_assert_nonnull(executable);
    test_executable = executable;
    g_test_init(&argc, &argv, NULL);
#define TEST(name, function) g_test_add("/settings-store/" name, StoreFixture, NULL, setup, function, teardown)
    TEST("missing-full-snapshot-alternation", missing_and_alternation);
    TEST("corruption-and-every-truncation", corrupt_slot_recovery);
    TEST("legacy-migration", legacy_migration);
    TEST("early-legacy-migration", early_legacy_migration);
    TEST("backup-only", backup_only);
    TEST("sequence-wrap", sequence_wrap);
    TEST("equal-and-ambiguous-sequences", equal_and_ambiguous_sequences);
    TEST("all-copies-bad", all_copies_bad);
    TEST("failed-write-preserves-latest", failed_write_preserves_latest);
    TEST("single-file-import-export", single_file_import_export);
    TEST("written-slot-permissions", written_slot_permissions);
    TEST("cross-process-lock-and-death", cross_process_lock);
    TEST("interrupted-writer", interrupted_writer);
#undef TEST
    return g_test_run();
}
