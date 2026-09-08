/* SPDX-License-Identifier: GPL-3.0-only */
#define PCRE2_CODE_UNIT_WIDTH 8
#include "log_model.h"
#include <pcre2.h>
#include <math.h>
#include <string.h>

#define MAX_LINES 10000
#define MAX_BYTES (8 * 1024 * 1024)
#define MAX_LINE 16384
struct _TioLogModel {
    GQueue entries;
    GByteArray *line;
    gsize bytes;
    guint64 next_id, revision, counts[TIO_LOG_LEVELS];
    gboolean cr, esc, csi, osc, osc_esc;
};
struct _TioLogFilter {
    pcre2_code *code;
    pcre2_match_data *match;
    pcre2_match_context *context;
    gchar *substring;
    gboolean sensitive;
    guint mask;
    guint64 limited;
};

static void entry_free(gpointer data)
{
    TioLogEntry *entry = data;
    g_free(entry->text);
    g_free(entry);
}

const char *tio_log_level_name(TioLogLevel level)
{
    static const char *names[] = {"CRITICAL", "ERROR", "WARNING", "INFO", "DEBUG", "COMMAND", "DATA"};
    return names[MIN((guint)level, TIO_LOG_LEVELS - 1)];
}

static TioLogLevel classify(const char *text)
{
    TioLogLevel level = TIO_LOG_DATA;
    g_autofree gchar *upper = g_ascii_strup(text, -1);
    g_auto(GStrv) words = g_strsplit_set(upper, " \t:;,[]{}()\"'=<>/\\.!?\r\n", -1);
    for (guint i = 0; words[i]; ++i) {
        const char *word = words[i];
        if (g_str_equal(word, "CRITICAL") || g_str_equal(word, "FATAL") || g_str_equal(word, "PANIC") || g_str_equal(word, "ASSERT"))
            level = MIN(level, TIO_LOG_CRITICAL);
        else if (g_str_equal(word, "ERROR") || g_str_equal(word, "FAIL") || g_str_equal(word, "FAILED"))
            level = MIN(level, TIO_LOG_ERROR);
        else if (g_str_equal(word, "WARN") || g_str_equal(word, "WARNING") || g_str_equal(word, "TIMEOUT") || g_str_equal(word, "RETRY"))
            level = MIN(level, TIO_LOG_WARNING);
        else if (g_str_equal(word, "INFO") || g_str_equal(word, "NOTICE") || g_str_equal(word, "OK") || g_str_equal(word, "READY"))
            level = MIN(level, TIO_LOG_INFO);
        else if (g_str_equal(word, "DEBUG") || g_str_equal(word, "TRACE")) level = MIN(level, TIO_LOG_DEBUG);
    }
    return level;
}

static void append(TioLogModel *model, const char *text, gint64 time_us, TioLogLevel level)
{
    TioLogEntry *entry = g_new0(TioLogEntry, 1);
    *entry = (TioLogEntry){++model->next_id, time_us, g_strdup(text), level};
    g_queue_push_tail(&model->entries, entry);
    model->bytes += strlen(text) + 1;
    ++model->counts[level];
    ++model->revision;
    while (model->entries.length > MAX_LINES || model->bytes > MAX_BYTES) {
        TioLogEntry *old = g_queue_pop_head(&model->entries);
        model->bytes -= strlen(old->text) + 1;
        entry_free(old);
    }
}

TioLogModel *tio_log_model_new(void)
{
    TioLogModel *model = g_new0(TioLogModel, 1);
    model->line = g_byte_array_sized_new(256);
    return model;
}
void tio_log_model_clear(TioLogModel *model)
{
    g_queue_clear_full(&model->entries, entry_free);
    g_byte_array_set_size(model->line, 0);
    model->bytes = 0;
    memset(model->counts, 0, sizeof model->counts);
    model->cr = model->esc = model->csi = model->osc = model->osc_esc = FALSE;
    ++model->revision;
}
void tio_log_model_free(TioLogModel *model)
{
    if (!model) return;
    tio_log_model_clear(model);
    g_byte_array_unref(model->line);
    g_free(model);
}
static void flush(TioLogModel *model, gint64 time_us)
{
    g_autofree gchar *text = g_utf8_make_valid((const char *)model->line->data, model->line->len);
    append(model, text, time_us, classify(text));
    g_byte_array_set_size(model->line, 0);
}
void tio_log_model_feed(TioLogModel *model, const guint8 *bytes, gsize length, gint64 time_us)
{
    for (gsize i = 0; i < length; ++i) {
        guint8 byte = bytes[i];
        if (model->osc) {
            if (byte == 7 || (model->osc_esc && byte == '\\')) model->osc = FALSE;
            model->osc_esc = byte == 27;
            continue;
        }
        if (model->csi) { if (byte >= 0x40 && byte <= 0x7e) model->csi = FALSE; continue; }
        if (model->esc) { model->esc = FALSE; model->csi = byte == '['; model->osc = byte == ']'; continue; }
        if (byte == 27) { model->esc = TRUE; continue; }
        if (byte == '\r') { flush(model, time_us); model->cr = TRUE; continue; }
        if (byte == '\n') { if (!model->cr) flush(model, time_us); model->cr = FALSE; continue; }
        model->cr = FALSE;
        if ((byte >= 32 || byte == '\t') && model->line->len < MAX_LINE)
            g_byte_array_append(model->line, &byte, 1);
    }
}
void tio_log_model_command(TioLogModel *model, const char *text, gint64 time_us)
{
    g_autofree gchar *valid = g_utf8_make_valid(text, (gssize)MIN(strlen(text), MAX_LINE));
    append(model, valid, time_us, TIO_LOG_COMMAND);
}
const GQueue *tio_log_model_entries(const TioLogModel *model) { return &model->entries; }
guint64 tio_log_model_count(const TioLogModel *model, TioLogLevel level) { return level < TIO_LOG_LEVELS ? model->counts[level] : 0; }
guint64 tio_log_model_revision(const TioLogModel *model) { return model->revision; }

/* Bound nesting before handing serial JSON to a recursive parser. */
static gboolean json_depth_ok(const char *text)
{
    guint depth = 0;
    gboolean string = FALSE, escape = FALSE;
    for (const char *p = text; *p; ++p) {
        if (string) {
            if (escape) escape = FALSE;
            else if (*p == '\\') escape = TRUE;
            else if (*p == '"') string = FALSE;
        } else if (*p == '"') string = TRUE;
        else if (*p == '{' || *p == '[') { if (++depth > 32) return FALSE; }
        else if (*p == '}' || *p == ']') { if (depth) --depth; }
    }
    return TRUE;
}
static void set_value(JsonObject *object, const char *key, const char *text)
{
    char *end = NULL;
    double value = g_ascii_strtod(text, &end);
    if (*text && end && !*end && isfinite(value)) json_object_set_double_member(object, key, value);
    else json_object_set_string_member(object, key, text);
}
JsonNode *tio_log_entry_fields(const TioLogEntry *entry)
{
    const char *text = entry->text;
    while (g_ascii_isspace(*text)) ++text;
    if ((*text == '{' || *text == '[') && json_depth_ok(text)) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, text, -1, NULL)) return json_node_copy(json_parser_get_root(parser));
    }
    JsonObject *object = json_object_new();
    /* Token scanning avoids an unbounded regex on arbitrary device output. */
    const char *p = text;
    guint fields = 0;
    while (*p && fields < 128) {
        if (!(g_ascii_isalpha(*p) || *p == '_')) { ++p; continue; }
        const char *start = p++;
        while (g_ascii_isalnum(*p) || *p == '_' || *p == '.' || *p == '-') ++p;
        const char *end = p;
        while (g_ascii_isspace(*p)) ++p;
        if (*p != ':' && *p != '=') continue;
        ++p;
        while (g_ascii_isspace(*p)) ++p;
        char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
        const char *value = p;
        while (*p && (quote ? *p != quote : (!g_ascii_isspace(*p) && *p != ',' && *p != ';'))) ++p;
        g_autofree gchar *key = g_strndup(start, (gsize)(end - start));
        g_autofree gchar *content = g_strndup(value, (gsize)(p - value));
        set_value(object, key, content);
        ++fields;
        if (quote && *p) ++p;
    }
    if (!fields && strchr(text, ',')) {
        const char *cursor = text;
        for (guint i = 0; i < 128; ++i) {
            g_autoptr(GString) cell = g_string_new(NULL);
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            gboolean quoted = *cursor == '"';
            if (quoted) ++cursor;
            while (*cursor) {
                if (quoted && *cursor == '"') {
                    if (cursor[1] == '"') { g_string_append_c(cell, '"'); cursor += 2; continue; }
                    ++cursor;
                    while (*cursor == ' ' || *cursor == '\t') ++cursor;
                    break;
                }
                if (!quoted && *cursor == ',') break;
                g_string_append_c(cell, *cursor++);
            }
            g_autofree gchar *key = g_strdup_printf("col%u", i + 1);
            set_value(object, key, quoted ? cell->str : g_strstrip(cell->str));
            if (*cursor != ',') break;
            ++cursor;
        }
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object);
    return node;
}
gboolean tio_log_entry_number(const TioLogEntry *entry, const char *field, double *value)
{
    g_autoptr(JsonNode) root = tio_log_entry_fields(entry);
    g_auto(GStrv) path = g_strsplit(field, ".", -1);
    JsonNode *node = root;
    if (JSON_NODE_HOLDS_OBJECT(root) && json_object_has_member(json_node_get_object(root), field)) {
        node = json_object_get_member(json_node_get_object(root), field);
    } else for (guint i = 0; path[i]; ++i) {
        if (JSON_NODE_HOLDS_OBJECT(node)) node = json_object_get_member(json_node_get_object(node), path[i]);
        else if (JSON_NODE_HOLDS_ARRAY(node)) {
            char *end;
            guint64 index = g_ascii_strtoull(path[i], &end, 10);
            JsonArray *array = json_node_get_array(node);
            if (!*path[i] || *end || index >= json_array_get_length(array)) return FALSE;
            node = json_array_get_element(array, (guint)index);
        } else return FALSE;
        if (!node) return FALSE;
    }
    if (!JSON_NODE_HOLDS_VALUE(node)) return FALSE;
    GType type = json_node_get_value_type(node);
    if (type == G_TYPE_DOUBLE || type == G_TYPE_INT64) { *value = json_node_get_double(node); return isfinite(*value); }
    if (type == G_TYPE_STRING) {
        const char *text = json_node_get_string(node);
        char *end;
        *value = g_ascii_strtod(text, &end);
        return *text && !*end && isfinite(*value);
    }
    return FALSE;
}

TioLogFilter *tio_log_filter_new(const char *pattern, gboolean regex, gboolean sensitive, guint mask, GError **error)
{
    if (strlen(pattern) > 1024) {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Filter exceeds 1024 bytes"); return NULL;
    }
    TioLogFilter *filter = g_new0(TioLogFilter, 1);
    filter->mask = mask;
    filter->sensitive = sensitive;
    if (regex && *pattern) {
        int code; PCRE2_SIZE offset;
        filter->code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
            PCRE2_UTF | PCRE2_UCP | (sensitive ? 0u : PCRE2_CASELESS), &code, &offset, NULL);
        if (!filter->code) {
            PCRE2_UCHAR message[256];
            pcre2_get_error_message(code, message, sizeof message);
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Regex at %zu: %s", (gsize)offset, message);
            tio_log_filter_free(filter); return NULL;
        }
        filter->context = pcre2_match_context_create(NULL);
        pcre2_set_match_limit(filter->context, 1000);
        pcre2_set_depth_limit(filter->context, 100);
        pcre2_set_heap_limit(filter->context, 1024);
        filter->match = pcre2_match_data_create_from_pattern(filter->code, NULL);
    } else filter->substring = sensitive ? g_strdup(pattern) : g_utf8_casefold(pattern, -1);
    return filter;
}
void tio_log_filter_free(TioLogFilter *filter)
{
    if (!filter) return;
    pcre2_code_free(filter->code); pcre2_match_data_free(filter->match); pcre2_match_context_free(filter->context);
    g_free(filter->substring); g_free(filter);
}
gboolean tio_log_filter_matches(TioLogFilter *filter, const TioLogEntry *entry)
{
    if (!filter) return TRUE;
    if (!(filter->mask & (1u << entry->level))) return FALSE;
    if (filter->code) {
        int rc = pcre2_match(filter->code, (PCRE2_SPTR)entry->text, strlen(entry->text), 0, 0, filter->match, filter->context);
        if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) ++filter->limited;
        return rc >= 0;
    }
    g_autofree gchar *text = filter->sensitive ? NULL : g_utf8_casefold(entry->text, -1);
    return strstr(text ? text : entry->text, filter->substring) != NULL;
}
guint64 tio_log_filter_limited(const TioLogFilter *filter) { return filter ? filter->limited : 0; }
static void csv_cell(GString *csv, const char *text)
{
    g_string_append_c(csv, '"');
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p && strchr("=+-@", *p)) g_string_append_c(csv, '\'');
    for (; *text; ++text) { if (*text == '"') g_string_append_c(csv, '"'); g_string_append_c(csv, *text); }
    g_string_append_c(csv, '"');
}
gchar *tio_log_csv_row(const TioLogEntry *entry, const char *field)
{
    GString *csv = g_string_new(NULL);
    g_string_append_printf(csv, "%" G_GINT64_FORMAT ",%s,", entry->time_us, tio_log_level_name(entry->level));
    csv_cell(csv, entry->text);
    if (field && *field) {
        double value;
        g_string_append_c(csv, ',');
        if (tio_log_entry_number(entry, field, &value)) {
            char buffer[G_ASCII_DTOSTR_BUF_SIZE];
            g_string_append(csv, g_ascii_dtostr(buffer, sizeof buffer, value));
        }
    }
    g_string_append_c(csv, '\n');
    return g_string_free(csv, FALSE);
}

gboolean tio_log_filter_number(TioLogFilter *filter, const TioLogEntry *entry,
                               const char *capture, double *value)
{
    if (!filter || !filter->code || !tio_log_filter_matches(filter, entry)) return FALSE;
    PCRE2_UCHAR *text = NULL;
    PCRE2_SIZE length;
    if (pcre2_substring_get_byname(filter->match, (PCRE2_SPTR)capture, &text, &length) < 0) return FALSE;
    char *end;
    *value = g_ascii_strtod((const char *)text, &end);
    gboolean ok = length > 0 && !*end && isfinite(*value);
    pcre2_substring_free(text);
    return ok;
}

static gchar *json_text(JsonNode *node)
{
    if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
        return g_strdup(json_node_get_string(node));
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, node);
    return json_generator_to_data(generator, NULL);
}

gchar *tio_log_model_csv(const TioLogModel *model, TioLogFilter *filter)
{
    g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);
    /* Limit discovery to the retained snapshot and at most 64 top-level fields.
     * Nested containers remain valid JSON in a quoted CSV cell. */
    for (GList *item = model->entries.head; item; item = item->next) {
        TioLogEntry *entry = item->data;
        if (!tio_log_filter_matches(filter, entry)) continue;
        g_autoptr(JsonNode) fields = tio_log_entry_fields(entry);
        if (!JSON_NODE_HOLDS_OBJECT(fields)) continue;
        GList *members = json_object_get_members(json_node_get_object(fields));
        for (GList *member = members; member && keys->len < 64; member = member->next)
            if (!g_ptr_array_find_with_equal_func(keys, member->data, (GEqualFunc)g_str_equal, NULL))
                g_ptr_array_add(keys, g_strdup(member->data));
        g_list_free(members);
    }
    GString *csv = g_string_new("timestamp_us,level,text");
    for (guint i = 0; i < keys->len; ++i) { g_string_append_c(csv, ','); csv_cell(csv, g_ptr_array_index(keys, i)); }
    g_string_append_c(csv, '\n');
    for (GList *item = model->entries.head; item; item = item->next) {
        TioLogEntry *entry = item->data;
        if (!tio_log_filter_matches(filter, entry)) continue;
        g_string_append_printf(csv, "%" G_GINT64_FORMAT ",%s,", entry->time_us, tio_log_level_name(entry->level));
        csv_cell(csv, entry->text);
        g_autoptr(JsonNode) fields = tio_log_entry_fields(entry);
        JsonObject *object = JSON_NODE_HOLDS_OBJECT(fields) ? json_node_get_object(fields) : NULL;
        for (guint i = 0; i < keys->len; ++i) {
            g_string_append_c(csv, ',');
            JsonNode *node = object ? json_object_get_member(object, g_ptr_array_index(keys, i)) : NULL;
            if (node) {
                g_autofree gchar *text = json_text(node);
                GType type = JSON_NODE_HOLDS_VALUE(node) ? json_node_get_value_type(node) : G_TYPE_INVALID;
                if (type == G_TYPE_DOUBLE || type == G_TYPE_INT64) g_string_append(csv, text);
                else csv_cell(csv, text);
            }
        }
        g_string_append_c(csv, '\n');
    }
    return g_string_free(csv, FALSE);
}
