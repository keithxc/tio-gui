/* SPDX-License-Identifier: GPL-3.0-only */
#include "dbc.h"
#include <json-glib/json-glib.h>
#include <string.h>
static void decode(void) {
  const char *text =
      "BO_ 291 Board: 8 ECU\n SG_ little : 0|16@1+ (1,0) [0|65535] \"\" ECU\n "
      "SG_ big : 23|16@0+ (1,0) [0|65535] \"\" ECU\n SG_ signed_scaled : "
      "32|8@1- (0.1,10) [-3|23] \"C\" ECU\n";
  TioDbc *dbc = tio_dbc_parse(text, strlen(text), NULL);
  g_assert_nonnull(dbc);
  guint8 bytes[] = {0x34, 0x12, 0x56, 0x78, 0xff, 0, 0, 0};
  g_autofree gchar *json =
      tio_dbc_decode(dbc, 291, FALSE, bytes, sizeof bytes, NULL);
  g_assert_nonnull(json);
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
  JsonObject *object = json_node_get_object(json_parser_get_root(parser));
  g_assert_cmpfloat(json_object_get_double_member(object, "little"), ==, 4660);
  g_assert_cmpfloat(json_object_get_double_member(object, "big"), ==, 22136);
  g_assert_cmpfloat_with_epsilon(
      json_object_get_double_member(object, "signed_scaled"), 9.9, 0.0001);
  g_assert_null(tio_dbc_decode(dbc, 291, FALSE, bytes, 4, NULL));
  tio_dbc_free(dbc);
  const char *mux = "BO_ 2147483939 Muxed: 8 ECU\n SG_ select M : 0|8@1+ (1,0) "
                    "[0|255] \"\" ECU\n SG_ one m1 : 8|8@1+ (1,0) [0|255] \"\" "
                    "ECU\n SG_ two m2 : 8|8@1+ (1,0) [0|255] \"\" ECU\n";
  dbc = tio_dbc_parse(mux, strlen(mux), NULL);
  g_assert_nonnull(dbc);
  bytes[0] = 2;
  bytes[1] = 42;
  g_autofree gchar *selected = tio_dbc_decode(dbc, 291, TRUE, bytes, 8, NULL);
  g_assert_nonnull(strstr(selected, "two"));
  g_assert_null(strstr(selected, "one"));
  tio_dbc_free(dbc);
  const char *bad[] = {"BO_ 1 Broken: 8 ECU\n SG_ out : 63|64@1+ (1,0)",
                       "BO_ 1 Float: 8 ECU\nSIG_VALTYPE_ 1 value : 1;",
                       "BO_ 1 Mux: 8 ECU\n SG_ missing m1 : 0|8@1+ (1,0)",
                       "BO_ 1 Bad: 8 ECU\n SG_ bad : 0|8@1+ (1e999,0)"};
  for (guint i = 0; i < G_N_ELEMENTS(bad); ++i)
    g_assert_null(tio_dbc_parse(bad[i], strlen(bad[i]), NULL));
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/dbc/endian-signed-scale-multiplex-bounds", decode);
  return g_test_run();
}
