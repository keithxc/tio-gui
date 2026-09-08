/* SPDX-License-Identifier: GPL-3.0-only */
#include "serial_options.h"
static void options(void)
{
    TioSessionConfig config;
    tio_session_config_init(&config);
    config.dtr_default = 2;
    config.rts_default = 1;
    config.rs485 = TRUE;
    g_free(config.rs485_config);
    config.rs485_config = g_strdup("RTS_ON_SEND=1, RTS_AFTER_SEND=0, RX_DURING_TX");
    g_assert_true(tio_serial_options_validate(&config, NULL));
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    tio_serial_options_append(argv, &config);
    g_assert_cmpstr(g_ptr_array_index(argv, 2), ==, "--script");
    g_assert_cmpstr(g_ptr_array_index(argv, 3), ==, "local api = tio or _G; api.set{DTR=1,RTS=0,}");
    g_assert_cmpstr(g_ptr_array_index(argv, argv->len - 1), ==,
                   "RTS_ON_SEND=1,RTS_AFTER_SEND=0,RX_DURING_TX");
    const char *invalid[] = {"RTS_ON_SEND=2", "RTS_DELAY_BEFORE_SEND=-1", "RX_DURING_TX,", "INVALID", "RTS_AFTER_SEND=1;echo x"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        g_free(config.rs485_config);
        config.rs485_config = g_strdup(invalid[i]);
        g_assert_false(tio_serial_options_validate(&config, NULL));
    }
    config.dtr_default = 9;
    g_assert_false(tio_serial_options_validate(&config, NULL));
    tio_session_config_clear(&config);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/serial-options/validation-and-argv", options);
    return g_test_run();
}
