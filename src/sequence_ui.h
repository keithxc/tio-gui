/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared Linux/native sequence editor and analyzer launcher. Backend adapters precede this include. */
typedef struct {
    GtkEntry *payload;
    GtkDropDown *mode, *ending, *crc;
    GtkSpinButton *delay;
    GtkWidget *box;
} SequenceRow;

typedef struct {
    TioTab *tab;
    GtkWidget *window;
    GtkEntry *name;
    GtkDropDown *saved;
    GtkBox *rows_box;
    GPtrArray *rows;
    GtkCheckButton *loop;
    GtkLabel *status;
    GtkButton *pause;
    guint timer;
} SequenceEditor;

static void sequence_row_action(GtkButton *button, gpointer data)
{
    SequenceEditor *editor = data;
    SequenceRow *row = g_object_get_data(G_OBJECT(button), "row");
    gint direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
    guint index;
    if (!g_ptr_array_find(editor->rows, row, &index)) return;
    if (!direction) {
        gtk_box_remove(editor->rows_box, row->box);
        g_ptr_array_remove_index(editor->rows, index);
        return;
    }
    if ((direction < 0 && index == 0) || (direction > 0 && index + 1 == editor->rows->len)) return;
    guint other = direction < 0 ? index - 1 : index + 1;
    gpointer temporary = g_ptr_array_index(editor->rows, other);
    g_ptr_array_index(editor->rows, other) = row;
    g_ptr_array_index(editor->rows, index) = temporary;
    GtkWidget *previous = NULL;
    for (guint i = 0; i < editor->rows->len; ++i) {
        SequenceRow *item = g_ptr_array_index(editor->rows, i);
        gtk_box_reorder_child_after(editor->rows_box, item->box, previous);
        previous = item->box;
    }
}

static void sequence_add_row(SequenceEditor *editor, const TioSequenceStep *step)
{
    if (editor->rows->len >= 256) return;
    SequenceRow *row = g_new0(SequenceRow, 1);
    row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    row->payload = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(row->payload, _("Payload (text escapes or HEX)"));
    gtk_widget_set_hexpand(GTK_WIDGET(row->payload), TRUE);
    const char *modes[] = {_("Text"), "HEX", NULL};
    const char *endings[] = {_("None"), "LF", "CR", "CRLF", NULL};
    const char *crcs[] = {_("None"), "CRC-8", "CRC-16/MODBUS", "CRC-32", NULL};
    row->mode = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes));
    row->ending = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(endings));
    row->crc = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(crcs));
    row->delay = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 60000, 10));
    gtk_widget_set_tooltip_text(GTK_WIDGET(row->delay), _("Delay after sending (ms)"));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->payload));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->mode));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->ending));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->crc));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->delay));
    const char *icons[] = {"go-up-symbolic", "go-down-symbolic", "edit-delete-symbolic"};
    const char *tips[] = {_("Move step up"), _("Move step down"), _("Remove step")};
    const gint directions[] = {-1, 1, 0};
    for (guint i = 0; i < 3; ++i) {
        GtkWidget *action = gtk_button_new_from_icon_name(icons[i]);
        gtk_widget_set_tooltip_text(action, tips[i]);
        g_object_set_data(G_OBJECT(action), "row", row);
        g_object_set_data(G_OBJECT(action), "direction", GINT_TO_POINTER(directions[i]));
        g_signal_connect(action, "clicked", G_CALLBACK(sequence_row_action), editor);
        gtk_box_append(GTK_BOX(row->box), action);
    }
    if (step) {
        gtk_editable_set_text(GTK_EDITABLE(row->payload), step->payload);
        gtk_drop_down_set_selected(row->mode, step->mode);
        gtk_drop_down_set_selected(row->ending, step->ending);
        gtk_drop_down_set_selected(row->crc, step->crc);
        gtk_spin_button_set_value(row->delay, step->delay_ms);
    } else gtk_spin_button_set_value(row->delay, 100);
    gtk_box_append(editor->rows_box, row->box);
    g_ptr_array_add(editor->rows, row);
}

static TioSequence *sequence_capture(SequenceEditor *editor)
{
    TioSequence *sequence = tio_sequence_new(gtk_editable_get_text(GTK_EDITABLE(editor->name)));
    for (guint i = 0; i < editor->rows->len; ++i) {
        SequenceRow *row = g_ptr_array_index(editor->rows, i);
        tio_sequence_add(sequence, gtk_editable_get_text(GTK_EDITABLE(row->payload)),
            gtk_drop_down_get_selected(row->mode), gtk_drop_down_get_selected(row->ending),
            gtk_drop_down_get_selected(row->crc), (guint)gtk_spin_button_get_value_as_int(row->delay));
    }
    g_autoptr(GError) error = NULL;
    if (!tio_sequence_validate(sequence, &error)) {
        gtk_label_set_text(editor->status, error->message);
        tio_sequence_free(sequence);
        return NULL;
    }
    return sequence;
}

static void sequence_refresh_saved(SequenceEditor *editor)
{
    GtkStringList *names = gtk_string_list_new(NULL);
    GPtrArray *saved = editor->tab->app->settings.sequences;
    for (guint i = 0; i < saved->len; ++i) {
        const char *text = g_ptr_array_index(saved, i);
        TioSequence *sequence = tio_sequence_decode(text, strlen(text), NULL);
        gtk_string_list_append(names, sequence ? sequence->name : _("Invalid sequence"));
        tio_sequence_free(sequence);
    }
    gtk_drop_down_set_model(editor->saved, G_LIST_MODEL(names));
    g_object_unref(names);
}

static void sequence_clear_rows(SequenceEditor *editor)
{
    for (guint i = 0; i < editor->rows->len; ++i) {
        SequenceRow *row = g_ptr_array_index(editor->rows, i);
        gtk_box_remove(editor->rows_box, row->box);
    }
    g_ptr_array_set_size(editor->rows, 0);
}

static void sequence_action(GtkButton *button, gpointer data)
{
    SequenceEditor *editor = data;
    TioTab *tab = editor->tab;
    const char *action = g_object_get_data(G_OBJECT(button), "sequence-action");
    GPtrArray *saved = tab->app->settings.sequences;
    guint selected = gtk_drop_down_get_selected(editor->saved);
    if (g_str_equal(action, "add")) { sequence_add_row(editor, NULL); return; }
    if (g_str_equal(action, "remove")) {
        if (editor->rows->len) {
            SequenceRow *row = g_ptr_array_index(editor->rows, editor->rows->len - 1);
            gtk_box_remove(editor->rows_box, row->box);
            g_ptr_array_remove_index(editor->rows, editor->rows->len - 1);
        }
        return;
    }
    if (g_str_equal(action, "new")) {
        sequence_clear_rows(editor);
        gtk_editable_set_text(GTK_EDITABLE(editor->name), _("New sequence"));
        sequence_add_row(editor, NULL);
        return;
    }
    if (g_str_equal(action, "load")) {
        if (selected >= saved->len) return;
        const char *text = g_ptr_array_index(saved, selected);
        g_autoptr(GError) error = NULL;
        TioSequence *sequence = tio_sequence_decode(text, strlen(text), &error);
        if (!sequence) { gtk_label_set_text(editor->status, error->message); return; }
        sequence_clear_rows(editor);
        gtk_editable_set_text(GTK_EDITABLE(editor->name), sequence->name);
        for (guint i = 0; i < sequence->steps->len; ++i)
            sequence_add_row(editor, g_ptr_array_index(sequence->steps, i));
        tio_sequence_free(sequence);
        return;
    }
    if (g_str_equal(action, "stop")) {
        g_clear_pointer(&tab->sequence_runner, tio_sequence_runner_free);
        gtk_label_set_text(editor->status, _("Stopped; bytes already sent cannot be recalled"));
        return;
    }
    if (g_str_equal(action, "pause")) {
        tab->sequence_paused = !tab->sequence_paused;
        tio_sequence_runner_pause(tab->sequence_runner, tab->sequence_paused);
        gtk_button_set_label(editor->pause, tab->sequence_paused ? _("Resume") : _("Pause"));
        return;
    }
    if (g_str_equal(action, "delete")) {
        if (selected >= saved->len) return;
        g_ptr_array_remove_index(saved, selected);
        sequence_refresh_saved(editor);
    } else {
        TioSequence *sequence = sequence_capture(editor);
        if (!sequence) return;
        if (g_str_equal(action, "run")) {
            if (tio_sequence_runner_active(tab->sequence_runner) || tab->quick_send_timer) {
                gtk_label_set_text(editor->status, _("Stop the current send before starting another"));
            } else if (!sequence_connected(tab)) {
                gtk_label_set_text(editor->status, _("Connect this session before running a sequence"));
            } else {
                g_clear_pointer(&tab->sequence_runner, tio_sequence_runner_free);
                tab->sequence_paused = FALSE;
                gtk_button_set_label(editor->pause, _("Pause"));
                tab->sequence_runner = tio_sequence_runner_new(sequence,
                    gtk_check_button_get_active(editor->loop), sequence_send, tab, NULL);
            }
            tio_sequence_free(sequence);
            return;
        }
        guint index;
        for (index = 0; index < saved->len; ++index) {
            const char *text = g_ptr_array_index(saved, index);
            TioSequence *old = tio_sequence_decode(text, strlen(text), NULL);
            gboolean match = old && g_str_equal(old->name, sequence->name);
            tio_sequence_free(old);
            if (match) break;
        }
        if (index == saved->len && saved->len >= 100) {
            gtk_label_set_text(editor->status, _("At most 100 sequences can be saved"));
            tio_sequence_free(sequence);
            return;
        }
        gchar *encoded = tio_sequence_encode(sequence, NULL);
        tio_sequence_free(sequence);
        if (index < saved->len) {
            g_free(g_ptr_array_index(saved, index));
            g_ptr_array_index(saved, index) = encoded;
        } else g_ptr_array_add(saved, encoded);
        sequence_refresh_saved(editor);
        gtk_drop_down_set_selected(editor->saved, index);
    }
    g_autoptr(GError) error = NULL;
    if (!sequence_save(tab, &error)) gtk_label_set_text(editor->status, error->message);
    else gtk_label_set_text(editor->status, _("Sequences saved"));
}

static gboolean sequence_ui_tick(gpointer data)
{
    SequenceEditor *editor = data;
    TioSequenceRunner *runner = editor->tab->sequence_runner;
    if (!runner) return G_SOURCE_CONTINUE;
    if (!tio_sequence_runner_active(runner)) {
        gtk_label_set_text(editor->status, tio_sequence_runner_failed(runner)
            ? _("Sequence stopped: transport failed") : _("Sequence completed"));
        g_clear_pointer(&editor->tab->sequence_runner, tio_sequence_runner_free);
    } else {
        g_autofree gchar *status = g_strdup_printf(editor->tab->sequence_paused
            ? _("Paused after step %u") : _("Running: %u steps sent in this pass"),
            tio_sequence_runner_step(runner));
        gtk_label_set_text(editor->status, status);
    }
    return G_SOURCE_CONTINUE;
}

static void sequence_editor_free(gpointer data)
{
    SequenceEditor *editor = data;
    if (editor->timer) g_source_remove(editor->timer);
    g_ptr_array_unref(editor->rows);
    g_free(editor);
}

static GtkWidget *sequence_button(SequenceEditor *editor, const char *label, const char *action)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(button), "sequence-action", (gpointer)action);
    g_signal_connect(button, "clicked", G_CALLBACK(sequence_action), editor);
    return button;
}

static void on_analyzer_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    TioTab *tab = data;
    if (!tab->analyzer_window) {
        tab->analyzer_window = tio_analyzer_new(GTK_WINDOW(tab->app->window), tab->log_model);
        g_object_add_weak_pointer(G_OBJECT(tab->analyzer_window), (gpointer *)&tab->analyzer_window);
    } else gtk_window_present(GTK_WINDOW(tab->analyzer_window));
}

static void on_sequences_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    TioTab *tab = data;
    if (tab->sequence_window) { gtk_window_present(GTK_WINDOW(tab->sequence_window)); return; }
    SequenceEditor *editor = g_new0(SequenceEditor, 1);
    editor->tab = tab;
    editor->rows = g_ptr_array_new_with_free_func(g_free);
    editor->window = gtk_window_new();
    tab->sequence_window = editor->window;
    g_object_add_weak_pointer(G_OBJECT(editor->window), (gpointer *)&tab->sequence_window);
    gtk_window_set_title(GTK_WINDOW(editor->window), _("Send sequences"));
    gtk_window_set_transient_for(GTK_WINDOW(editor->window), GTK_WINDOW(tab->app->window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(editor->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(editor->window), 980, 520);
    g_object_set_data_full(G_OBJECT(editor->window), "sequence-editor", editor, sequence_editor_free);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(editor->window), root);
    GtkWidget *saved = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    editor->saved = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(editor->saved), TRUE);
    gtk_box_append(GTK_BOX(saved), GTK_WIDGET(editor->saved));
    gtk_box_append(GTK_BOX(saved), sequence_button(editor, _("Load"), "load"));
    gtk_box_append(GTK_BOX(saved), sequence_button(editor, _("New"), "new"));
    gtk_box_append(GTK_BOX(saved), sequence_button(editor, _("Delete sequence"), "delete"));
    gtk_box_append(GTK_BOX(root), saved);
    editor->name = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(editor->name), _("New sequence"));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(editor->name));
    GtkWidget *hint = gtk_label_new(_("Steps run top to bottom: payload, mode, line ending, CRC, delay after send (ms)."));
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_append(GTK_BOX(root), hint);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    editor->rows_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(editor->rows_box));
    gtk_box_append(GTK_BOX(root), scroll);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Add step"), "add"));
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Remove last"), "remove"));
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Save sequence"), "save"));
    editor->loop = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Loop")));
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(editor->loop));
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Run"), "run"));
    editor->pause = GTK_BUTTON(sequence_button(editor, _("Pause"), "pause"));
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(editor->pause));
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Stop"), "stop"));
    gtk_box_append(GTK_BOX(root), actions);
    editor->status = GTK_LABEL(gtk_label_new(_("Disconnect stops the sequence. Closing this editor leaves it running.")));
    gtk_label_set_wrap(editor->status, TRUE);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(editor->status));
    sequence_add_row(editor, NULL);
    sequence_refresh_saved(editor);
    editor->timer = g_timeout_add(100, sequence_ui_tick, editor);
    gtk_window_present(GTK_WINDOW(editor->window));
}
