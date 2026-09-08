/* SPDX-License-Identifier: GPL-3.0-only */
#include "quick_presets.h"
#include <glib/gstdio.h>
#include <string.h>
static void roundtrip(void)
{
    TioSettings source, restored;
    tio_settings_init(&source);
    tio_settings_init(&restored);
    TioSessionConfig *config = &source.defaults;
    g_free(config->quick_payloads[0]);
    config->quick_payloads[0] = g_strdup("01 03 00 00 00 0A");
    config->quick_modes[0] = 1;
    config->quick_crcs[0] = 2;
    config->quick_endings[0] = 3;
    config->quick_delays[0] = 170;
    gsize length;
    g_autofree gchar *encoded = tio_quick_presets_encode(config, &length);
    g_assert_null(strstr(encoded, "log-directory"));
    g_assert_null(strstr(encoded, "device="));
    g_assert_true(tio_quick_presets_decode(&restored.defaults, encoded, length, NULL));
    g_assert_cmpstr(restored.defaults.quick_payloads[0], ==, config->quick_payloads[0]);
    g_assert_cmpuint(restored.defaults.quick_crcs[0], ==, 2);
    g_assert_cmpuint(restored.defaults.quick_delays[0], ==, 170);
    /* A corrupt later row must leave every previous row unchanged. */
    g_autofree gchar *bad = g_strdup(encoded);
    char *version = strstr(bad, "delay=0");
    g_assert_nonnull(version);
    memcpy(version, "delay=x", 7);
    g_autoptr(GError) error = NULL;
    g_assert_false(tio_quick_presets_decode(&restored.defaults, bad, length, &error));
    g_assert_nonnull(error);
    g_assert_cmpstr(restored.defaults.quick_payloads[0], ==, config->quick_payloads[0]);
    g_autofree gchar *directory = g_dir_make_tmp("tio-settings-test-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(directory, "config.ini", NULL);
    g_assert_true(tio_settings_save_to_file(&source, path, NULL));
    g_assert_true(tio_settings_load_from_file(&restored, path, NULL));
    g_assert_cmpuint(restored.defaults.quick_modes[0], ==, 1);
    g_assert_cmpuint(restored.defaults.quick_endings[0], ==, 3);
    g_assert_cmpuint(restored.defaults.quick_crcs[0], ==, 2);
    g_assert_cmpuint(restored.defaults.quick_delays[0], ==, 170);
    g_free(source.defaults.device);
    source.defaults.device = g_strdup("/dev/serial/by-id/private-device");
    g_free(source.defaults.log_directory);
    source.defaults.log_directory = g_strdup("/home/private-user/private-logs");
    tio_settings_push_history(&source, "private-command-history");
    tio_settings_store_profile(&source, "Board", &source.defaults);
    g_assert_true(tio_settings_export_portable(&source, path, NULL));
    g_autofree gchar *portable = NULL;
    g_assert_true(g_file_get_contents(path, &portable, NULL, NULL));
    g_assert_null(strstr(portable, "private-device"));
    g_assert_null(strstr(portable, "private-user"));
    g_assert_null(strstr(portable, "private-command-history"));
    g_assert_nonnull(strstr(portable, "profile:Board"));
    g_assert_cmpstr(source.defaults.log_directory, ==, "/home/private-user/private-logs");
    g_unlink(path);
    g_rmdir(directory);
    tio_settings_clear(&source);
    tio_settings_clear(&restored);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/quick-presets/roundtrip-atomic-validation", roundtrip);
    return g_test_run();
}
