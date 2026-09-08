/* SPDX-License-Identifier: GPL-3.0-only */
#include "dbc.h"
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
typedef struct {
  gchar *name;
  guint start, bits;
  gboolean little, sign, selector;
  gint mux;
  double factor, offset;
} Signal;
typedef struct {
  gchar *name;
  guint32 id;
  gboolean extended;
  guint length;
  GPtrArray *signals;
} Message;
struct _TioDbc {
  GPtrArray *messages;
};
static void signal_free(gpointer data) {
  Signal *signal = data;
  g_free(signal->name);
  g_free(signal);
}
static void message_free(gpointer data) {
  Message *message = data;
  g_free(message->name);
  g_ptr_array_unref(message->signals);
  g_free(message);
}
void tio_dbc_free(TioDbc *dbc) {
  if (dbc) {
    g_ptr_array_unref(dbc->messages);
    g_free(dbc);
  }
}
static guint64 integer(GMatchInfo *match, guint group) {
  g_autofree gchar *text = g_match_info_fetch(match, (gint)group);
  return g_ascii_strtoull(text, NULL, 10);
}
static double number(GMatchInfo *match, guint group) {
  g_autofree gchar *text = g_match_info_fetch(match, (gint)group);
  char *end;
  double value = g_ascii_strtod(text, &end);
  return *end ? NAN : value;
}
static gboolean fits(Signal *signal, guint bytes) {
  guint position = signal->start;
  for (guint i = 0; i < signal->bits; ++i) {
    if (position >= bytes * 8)
      return FALSE;
    position = signal->little ? position + 1
               : position % 8 ? position - 1
                              : position + 15;
  }
  return TRUE;
}
TioDbc *tio_dbc_parse(const char *text, gsize length, GError **error) {
  if (!text || length > 4194304 || memchr(text, 0, length) ||
      !g_utf8_validate(text, (gssize)length, NULL)) {
    g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                        "DBC must be UTF-8 text up to 4 MiB");
    return NULL;
  }
  g_autofree gchar *copy = g_strndup(text, length);
  g_auto(GStrv) lines = g_strsplit(copy, "\n", -1);
  g_autoptr(GRegex) bo = g_regex_new("^BO_\\s+([0-9]{1,10})\\s+([A-Za-z_][A-Za-"
                                     "z0-9_]{0,127})\\s*:\\s*([0-9]{1,2})\\s+",
                                     0, 0, NULL);
  g_autoptr(GRegex) sg =
      g_regex_new("^SG_\\s+([A-Za-z_][A-Za-z0-9_]{0,127})(?:\\s+(M|m[0-9]{1,5})"
                  ")?\\s*:\\s*([0-9]{1,3})\\|([0-9]{1,2})@([01])([+-])\\s*\\((["
                  "-+0-9.eE]{1,32}),\\s*([-+0-9.eE]{1,32})\\)",
                  0, 0, NULL);
  TioDbc *dbc = g_new0(TioDbc, 1);
  dbc->messages = g_ptr_array_new_with_free_func(message_free);
  Message *current = NULL;
  guint line = 0;
  for (; lines[line]; ++line) {
    gchar *value = g_strstrip(lines[line]);
    g_autoptr(GMatchInfo) match = NULL;
    if (strlen(value) > 4096)
      goto invalid;
    if ((g_str_has_prefix(value, "BO_") && g_ascii_isspace(value[3]))) {
      if (dbc->messages->len >= 1024 || !g_regex_match(bo, value, 0, &match))
        goto invalid;
      guint64 id = integer(match, 1), size = integer(match, 3);
      if (id > 0xffffffffu || size > 64 || (id & 0x60000000u) ||
          (!(id & 0x80000000u) && id > 2047))
        goto invalid;
      for (guint i = 0; i < dbc->messages->len; ++i) {
        Message *other = g_ptr_array_index(dbc->messages, i);
        if (other->id == (id & 0x1fffffffu) &&
            other->extended == !!(id & 0x80000000u))
          goto invalid;
      }
      current = g_new0(Message, 1);
      current->id = (guint32)(id & 0x1fffffffu);
      current->extended = !!(id & 0x80000000u);
      current->length = (guint)size;
      current->name = g_match_info_fetch(match, 2);
      current->signals = g_ptr_array_new_with_free_func(signal_free);
      g_ptr_array_add(dbc->messages, current);
    } else if ((g_str_has_prefix(value, "SG_") && g_ascii_isspace(value[3]))) {
      if (!current || current->signals->len >= 128 ||
          !g_regex_match(sg, value, 0, &match))
        goto invalid;
      Signal *signal = g_new0(Signal, 1);
      signal->name = g_match_info_fetch(match, 1);
      signal->start = (guint)integer(match, 3);
      signal->bits = (guint)integer(match, 4);
      signal->little = integer(match, 5) == 1;
      g_autofree gchar *sign = g_match_info_fetch(match, 6),
                       *mux = g_match_info_fetch(match, 2);
      signal->sign = *sign == '-';
      signal->factor = number(match, 7);
      signal->offset = number(match, 8);
      signal->mux = -1;
      if (*mux == 'M')
        signal->selector = TRUE;
      else if (*mux == 'm')
        signal->mux = (gint)g_ascii_strtoll(mux + 1, NULL, 10);
      g_ptr_array_add(current->signals, signal);
      if (!signal->bits || signal->bits > 64 ||
          !fits(signal, current->length) || !isfinite(signal->factor) ||
          !isfinite(signal->offset))
        goto invalid;
      for (guint i = 0; i + 1 < current->signals->len; ++i) {
        Signal *other = g_ptr_array_index(current->signals, i);
        if (g_str_equal(signal->name, other->name) ||
            (signal->selector && other->selector))
          goto invalid;
      }
    } else if (g_str_has_prefix(value, "SG_MUL_VAL_") ||
               g_str_has_prefix(value, "SIG_VALTYPE_"))
      goto invalid;
  }
  for (guint i = 0; i < dbc->messages->len; ++i) {
    Message *message = g_ptr_array_index(dbc->messages, i);
    gboolean selector = FALSE, mux = FALSE;
    for (guint j = 0; j < message->signals->len; ++j) {
      Signal *signal = g_ptr_array_index(message->signals, j);
      selector |= signal->selector;
      mux |= signal->mux >= 0;
    }
    if (mux && !selector)
      goto invalid;
  }
  if (!dbc->messages->len)
    goto invalid;
  return dbc;
invalid:
  g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
              "Invalid or unsupported DBC definition near line %u (integer "
              "signals and simple multiplexing supported)",
              line + 1);
  tio_dbc_free(dbc);
  return NULL;
}
static guint64 extract(Signal *signal, const guint8 *bytes) {
  guint position = signal->start;
  guint64 raw = 0;
  for (guint i = 0; i < signal->bits; ++i) {
    guint64 bit = (bytes[position / 8] >> (position % 8)) & 1u;
    if (signal->little)
      raw |= bit << i;
    else
      raw = (raw << 1) | bit;
    position = signal->little ? position + 1
               : position % 8 ? position - 1
                              : position + 15;
  }
  return raw;
}
gchar *tio_dbc_decode(TioDbc *dbc, guint32 id, gboolean extended,
                      const guint8 *bytes, gsize length, GError **error) {
  if (!dbc)
    return NULL;
  Message *message = NULL;
  for (guint i = 0; i < dbc->messages->len; ++i) {
    Message *candidate = g_ptr_array_index(dbc->messages, i);
    if (candidate->id == id && candidate->extended == extended) {
      message = candidate;
      break;
    }
  }
  if (!message)
    return NULL;
  if (length < message->length || (length && !bytes)) {
    g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                        "CAN payload is shorter than its DBC message");
    return NULL;
  }
  guint64 mux = 0;
  for (guint i = 0; i < message->signals->len; ++i) {
    Signal *signal = g_ptr_array_index(message->signals, i);
    if (signal->selector)
      mux = extract(signal, bytes);
  }
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "message");
  json_builder_add_string_value(builder, message->name);
  for (guint i = 0; i < message->signals->len; ++i) {
    Signal *signal = g_ptr_array_index(message->signals, i);
    if (signal->mux >= 0 && (guint64)signal->mux != mux)
      continue;
    guint64 raw = extract(signal, bytes);
    double value = (double)raw;
    if (signal->sign && (raw & ((guint64)1 << (signal->bits - 1))))
      value = signal->bits == 64
                  ? -(double)(~raw + 1)
                  : (double)raw - (double)((guint64)1 << signal->bits);
    value = value * signal->factor + signal->offset;
    if (!isfinite(value)) {
      g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                          "DBC scaled value is not finite");
      return NULL;
    }
    json_builder_set_member_name(builder, signal->name);
    json_builder_add_double_value(builder, value);
  }
  json_builder_end_object(builder);
  g_autoptr(JsonNode) root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);
  return json_generator_to_data(generator, NULL);
}
