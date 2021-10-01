/*
    Copyright (C) 2011  ABRT Team
    Copyright (C) 2011  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include "client.h"
#include "internal_libreport_gtk.h"
#include "wizard.h"
#include "search_item.h"
#include "libreport_types.h"
#include "global_configuration.h"

#define DEFAULT_WIDTH   800
#define DEFAULT_HEIGHT  500

#define EMERGENCY_ANALYSIS_EVENT_NAME "report_EmergencyAnalysis"
#define FORBIDDEN_WORDS_BLACKLLIST "forbidden_words.conf"
#define FORBIDDEN_WORDS_WHITELIST "ignored_words.conf"

#if GTK_MAJOR_VERSION == 2 && GTK_MINOR_VERSION < 22
# define gtk_assistant_commit(...) ((void)0)
# define GDK_KEY_Delete GDK_Delete
# define GDK_KEY_KP_Delete GDK_KP_Delete
#endif

/* For Fedora 16 and gtk3 < 3.4.4*/
#ifndef GDK_BUTTON_PRIMARY
# define GDK_BUTTON_PRIMARY 1
#endif

typedef struct event_gui_data_t
{
    char *event_name;
    GtkToggleButton *toggle_button;
} event_gui_data_t;


/* Using GHashTable as a set of file names */
/* Each table key has associated an nonzero integer and it allows us */
/* to write the following statements:                                */
/*   if(g_hash_table_lookup(g_loaded_texts, FILENAME_COMMENT)) ...   */
static GHashTable *g_loaded_texts;
static char *g_event_selected;
static unsigned g_black_event_count = 0;

static pid_t g_event_child_pid = 0;
static guint g_event_source_id = 0;

static bool g_expert_mode;

static GtkNotebook *g_assistant;
static GtkWindow *g_wnd_assistant;
static GtkBox *g_box_assistant;

static GtkWidget *g_btn_stop;
static GtkWidget *g_btn_close;
static GtkWidget *g_btn_next;
static GtkWidget *g_btn_onfail;
static GtkWidget *g_btn_repeat;
static GtkWidget *g_btn_detail;

static GtkBox *g_box_events;
static GtkBox *g_box_workflows;
/* List of event_gui_data's */
static GList *g_list_events;
static GtkLabel *g_lbl_event_log;
static GtkTextView *g_tv_event_log;

/* List of event_gui_data's */

/* List of event_gui_data's */
static GtkContainer *g_container_details1;
static GtkContainer *g_container_details2;

static GtkLabel *g_lbl_cd_reason;
static GtkTextView *g_tv_comment;
static GtkEventBox *g_eb_comment;
static GtkCheckButton *g_cb_no_comment;
static GtkWidget *g_widget_warnings_area;
static GtkBox *g_box_warning_labels;
static GtkToggleButton *g_tb_approve_bt;
static GtkButton *g_btn_add_file;

static GtkLabel *g_lbl_size;

static GtkTreeView *g_tv_details;
static GtkCellRenderer *g_tv_details_renderer_value;
static GtkTreeViewColumn *g_tv_details_col_checkbox;
//static GtkCellRenderer *g_tv_details_renderer_checkbox;
static GtkListStore *g_ls_details;

static GtkBox *g_box_buttons; //TODO: needs not be global
static GtkNotebook *g_notebook;
static GtkListStore *g_ls_sensitive_list;
static GtkTreeView *g_tv_sensitive_list;
static GtkTreeSelection *g_tv_sensitive_sel;
static GtkRadioButton *g_rb_forbidden_words;
static GtkRadioButton *g_rb_custom_search;
static GtkExpander *g_exp_search;
static gulong g_tv_sensitive_sel_hndlr;
static gboolean g_warning_issued;

static GtkSpinner *g_spinner_event_log;
static GtkImage *g_img_process_fail;

static GtkButton *g_btn_startcast;
static GtkExpander *g_exp_report_log;

static GtkWidget *g_top_most_window;

static void add_workflow_buttons(GtkBox *box, GHashTable *workflows, GCallback func);
static void set_auto_event_chain(GtkButton *button, gpointer user_data);
static void start_event_run(const char *event_name);

enum
{
    /* Note: need to update types in
     * gtk_list_store_new(DETAIL_NUM_COLUMNS, TYPE1, TYPE2...)
     * if you change these:
     */
    DETAIL_COLUMN_CHECKBOX,
    DETAIL_COLUMN_NAME,
    DETAIL_COLUMN_VALUE,
    DETAIL_NUM_COLUMNS,
};

/* Search in bt */
static guint g_timeout = 0;
static GtkEntry *g_search_entry_bt;
static const gchar *g_search_text;
static search_item_t *g_current_highlighted_word;

enum
{
    SEARCH_COLUMN_FILE,
    SEARCH_COLUMN_TEXT,
    SEARCH_COLUMN_ITEM,
};

static GtkBuilder *g_builder;
static PangoFontDescription *g_monospace_font;

/* THE PAGE FLOW
 * page_0: introduction/summary
 * page_1: user comments
 * page_2: event selection
 * page_3: backtrace editor
 * page_4: summary
 * page_5: reporting progress
 * page_6: finished
 */
enum {
    PAGENO_SUMMARY,        // 0
    PAGENO_EVENT_SELECTOR, // 1
    PAGENO_EDIT_COMMENT,   // 2
    PAGENO_EDIT_ELEMENTS,  // 3
    PAGENO_REVIEW_DATA,    // 4
    PAGENO_EVENT_PROGRESS, // 5
    PAGENO_EVENT_DONE,     // 6
    PAGENO_NOT_SHOWN,      // 7
    NUM_PAGES              // 8
};

/* Use of arrays (instead of, say, #defines to C strings)
 * allows cheaper page_obj_t->name == PAGE_FOO comparisons
 * instead of strcmp.
 */
static const gchar PAGE_SUMMARY[]        = "page_0";
static const gchar PAGE_EVENT_SELECTOR[] = "page_1";
static const gchar PAGE_EDIT_COMMENT[]   = "page_2";
static const gchar PAGE_EDIT_ELEMENTS[]  = "page_3";
static const gchar PAGE_REVIEW_DATA[]    = "page_4";
static const gchar PAGE_EVENT_PROGRESS[] = "page_5";
static const gchar PAGE_EVENT_DONE[]     = "page_6";
static const gchar PAGE_NOT_SHOWN[]      = "page_7";

static const gchar *const page_names[] =
{
    PAGE_SUMMARY,
    PAGE_EVENT_SELECTOR,
    PAGE_EDIT_COMMENT,
    PAGE_EDIT_ELEMENTS,
    PAGE_REVIEW_DATA,
    PAGE_EVENT_PROGRESS,
    PAGE_EVENT_DONE,
    PAGE_NOT_SHOWN,
    NULL
};

#define PRIVATE_TICKET_CB "private_ticket_cb"

#define SENSITIVE_DATA_WARN "sensitive_data_warning"
#define SENSITIVE_LIST "ls_sensitive_words"
static const gchar *misc_widgets[] =
{
    SENSITIVE_DATA_WARN,
    SENSITIVE_LIST,
    NULL
};

typedef struct
{
    const gchar *name;
    const gchar *title;
    GtkWidget *page_widget;
    int page_no;
} page_obj_t;

static page_obj_t pages[NUM_PAGES];

static struct strbuf *cmd_output = NULL;

/* Utility functions */

static void clear_warnings(void);
static void show_warnings(void);
static void add_warning(const char *warning);
static bool check_minimal_bt_rating(const char *event_name);
static char *get_next_processed_event(GList **events_list);
static void on_next_btn_cb(GtkWidget *btn, gpointer user_data);

/* wizard.glade file as a string WIZARD_GLADE_CONTENTS: */
#include "wizard_glade.c"

static GtkBuilder *make_builder()
{
    GError *error = NULL;
    GtkBuilder *builder = gtk_builder_new();
    if (!g_glade_file)
    {
        /* load additional widgets from glade */
        gtk_builder_add_objects_from_string(builder,
                WIZARD_GLADE_CONTENTS, sizeof(WIZARD_GLADE_CONTENTS) - 1,
                (gchar**)misc_widgets,
                &error);
        if (error != NULL)
            error_msg_and_die("Error loading glade data: %s", error->message);
        /* Load pages from internal string */
        gtk_builder_add_objects_from_string(builder,
                WIZARD_GLADE_CONTENTS, sizeof(WIZARD_GLADE_CONTENTS) - 1,
                (gchar**)page_names,
                &error);
        if (error != NULL)
            error_msg_and_die("Error loading glade data: %s", error->message);
    }
    else
    {
        /* -g FILE: load UI from it */
        /* load additional widgets from glade */
        gtk_builder_add_objects_from_file(builder, g_glade_file, (gchar**)misc_widgets, &error);
        if (error != NULL)
            error_msg_and_die("Can't load %s: %s", g_glade_file, error->message);
        gtk_builder_add_objects_from_file(builder, g_glade_file, (gchar**)page_names, &error);
        if (error != NULL)
            error_msg_and_die("Can't load %s: %s", g_glade_file, error->message);
    }

    return builder;
}

static void label_wrapper(GtkWidget *widget, gpointer data_unused)
{
    if (GTK_IS_CONTAINER(widget))
    {
        gtk_container_foreach((GtkContainer*)widget, label_wrapper, NULL);
        return;
    }
    if (GTK_IS_LABEL(widget))
    {
        GtkLabel *label = (GtkLabel*)widget;
        gtk_label_set_line_wrap(label, 1);
        //const char *txt = gtk_label_get_label(label);
        //log("label '%s' set to wrap", txt);
    }
}

static void wrap_all_labels(GtkWidget *widget)
{
    label_wrapper(widget, NULL);
}

static void wrap_fixer(GtkWidget *widget, gpointer data_unused)
{
    if (GTK_IS_CONTAINER(widget))
    {
        gtk_container_foreach((GtkContainer*)widget, wrap_fixer, NULL);
        return;
    }
    if (GTK_IS_LABEL(widget))
    {
        GtkLabel *label = (GtkLabel*)widget;
        //const char *txt = gtk_label_get_label(label);
#if ((GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 13) || (GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION == 13 && GTK_MICRO_VERSION < 5))
        GtkMisc *misc = (GtkMisc*)widget;
        gfloat yalign; // = 111;
        gint ypad; // = 111;
        if (gtk_label_get_line_wrap(label)
         && (gtk_misc_get_alignment(misc, NULL, &yalign), yalign == 0)
         && (gtk_misc_get_padding(misc, NULL, &ypad), ypad == 0)
#else
        if (gtk_label_get_line_wrap(label)
         && (gtk_widget_get_halign(widget) == GTK_ALIGN_START)
         && (gtk_widget_get_margin_top(widget) == 0)
         && (gtk_widget_get_margin_bottom(widget) == 0)
#endif
        ) {
            //log("label '%s' set to autowrap", txt);
            make_label_autowrap_on_resize(label);
            return;
        }
        //log("label '%s' not set to autowrap %g %d", txt, yalign, ypad);
    }
}

static void fix_all_wrapped_labels(GtkWidget *widget)
{
    wrap_fixer(widget, NULL);
}

static void remove_child_widget(GtkWidget *widget, gpointer unused)
{
    /* Destroy will safely remove it and free the memory
     * if there are no refs left
     */
    gtk_widget_destroy(widget);
}


static void update_window_title(void)
{
    /* prgname can be null according to gtk documentation */
    const char *prgname = g_get_prgname();
    const char *reason = problem_data_get_content_or_NULL(g_cd, FILENAME_REASON);
    char *title = xasprintf("%s - %s", (reason ? reason : g_dump_dir_name),
            (prgname ? prgname : "report"));
    gtk_window_set_title(g_wnd_assistant, title);
    free(title);
}

static bool ask_continue_before_steal(const char *base_dir, const char *dump_dir)
{
    char *msg = xasprintf(_("Need writable directory, but '%s' is not writable."
                            " Move it to '%s' and operate on the moved data?"),
                            dump_dir, base_dir);
    const bool response = run_ask_yes_no_yesforever_dialog("ask_steal_dir", msg, GTK_WINDOW(g_wnd_assistant));
    free(msg);
    return response;
}

struct dump_dir *wizard_open_directory_for_writing(const char *dump_dir_name)
{
    struct dump_dir *dd = open_directory_for_writing(dump_dir_name,
                                                     ask_continue_before_steal);

    if (dd && strcmp(g_dump_dir_name, dd->dd_dirname) != 0)
    {
        char *old_name = g_dump_dir_name;
        g_dump_dir_name = xstrdup(dd->dd_dirname);
        update_window_title();
        free(old_name);
    }

    return dd;
}

void show_error_as_msgbox(const char *msg)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_wnd_assistant),
                GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_WARNING,
                GTK_BUTTONS_CLOSE,
                "%s", msg
    );
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void load_text_to_text_view(GtkTextView *tv, const char *name)
{
    /* Add to set of loaded files */
    /* a key_destroy_func() is provided therefore if the key for name already exists */
    /* a result of xstrdup() is freed */
    g_hash_table_insert(g_loaded_texts, (gpointer)xstrdup(name), (gpointer)1);

    const char *str = g_cd ? problem_data_get_content_or_NULL(g_cd, name) : NULL;
    /* Bad: will choke at any text with non-Unicode parts: */
    /* gtk_text_buffer_set_text(tb, (str ? str : ""), -1);*/
    /* Start torturing ourself instead: */

    reload_text_to_text_view(tv, str);
}

static gchar *get_malloced_string_from_text_view(GtkTextView *tv)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(tv);
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void save_text_if_changed(const char *name, const char *new_value)
{
    /* a text value can't be change if the file is not loaded */
    /* returns NULL if the name is not found; otherwise nonzero */
    if (!g_hash_table_lookup(g_loaded_texts, name))
        return;

    const char *old_value = g_cd ? problem_data_get_content_or_NULL(g_cd, name) : "";
    if (!old_value)
        old_value = "";
    if (strcmp(new_value, old_value) != 0)
    {
        struct dump_dir *dd = wizard_open_directory_for_writing(g_dump_dir_name);
        if (dd)
            dd_save_text(dd, name, new_value);

//FIXME: else: what to do with still-unsaved data in the widget??
        dd_close(dd);
    }
}

static void save_text_from_text_view(GtkTextView *tv, const char *name)
{
    gchar *new_str = get_malloced_string_from_text_view(tv);
    save_text_if_changed(name, new_str);
    free(new_str);
}

static void append_to_textview(GtkTextView *tv, const char *str)
{
    GtkTextBuffer *tb = gtk_text_view_get_buffer(tv);

    /* Ensure we insert text at the end */
    GtkTextIter text_iter;
    gtk_text_buffer_get_end_iter(tb, &text_iter);
    gtk_text_buffer_place_cursor(tb, &text_iter);

    /* Deal with possible broken Unicode */
    const gchar *end;
    while (!g_utf8_validate(str, -1, &end))
    {
        gtk_text_buffer_insert_at_cursor(tb, str, end - str);
        char buf[8];
        unsigned len = snprintf(buf, sizeof(buf), "<%02X>", (unsigned char)*end);
        gtk_text_buffer_insert_at_cursor(tb, buf, len);
        str = end + 1;
    }

    gtk_text_buffer_get_end_iter(tb, &text_iter);

    const char *last = str;
    GList *urls = find_url_tokens(str);
    for (GList *u = urls; u; u = g_list_next(u))
    {
        const struct url_token *const t = (struct url_token *)u->data;
        if (last < t->start)
            gtk_text_buffer_insert(tb, &text_iter, last, t->start - last);

        GtkTextTag *tag;
        tag = gtk_text_buffer_create_tag (tb, NULL, "foreground", "blue",
                                          "underline", PANGO_UNDERLINE_SINGLE, NULL);
        char *url = xstrndup(t->start, t->len);
        g_object_set_data (G_OBJECT (tag), "url", url);

        gtk_text_buffer_insert_with_tags(tb, &text_iter, url, -1, tag, NULL);

        last = t->start + t->len;
    }

    g_list_free_full(urls, g_free);

    if (last[0] != '\0')
        gtk_text_buffer_insert(tb, &text_iter, last, strlen(last));

    /* Scroll so that the end of the log is visible */
    gtk_text_view_scroll_to_iter(tv, &text_iter,
                /*within_margin:*/ 0.0, /*use_align:*/ FALSE, /*xalign:*/ 0, /*yalign:*/ 0);
}

/* Looks at all tags covering the position of iter in the text view,
 * and if one of them is a link, follow it by showing the page identified
 * by the data attached to it.
 */
static void open_browse_if_link(GtkWidget *text_view, GtkTextIter *iter)
{
    GSList *tags = NULL, *tagp = NULL;

    tags = gtk_text_iter_get_tags (iter);
    for (tagp = tags;  tagp != NULL;  tagp = tagp->next)
    {
        GtkTextTag *tag = tagp->data;
        const char *url = g_object_get_data (G_OBJECT (tag), "url");

        if (url != 0)
        {
            /* http://techbase.kde.org/KDE_System_Administration/Environment_Variables#KDE_FULL_SESSION */
            if (getenv("KDE_FULL_SESSION") != NULL)
            {
                gint exitcode;
                gchar *arg[3];
                /* kde-open is from kdebase-runtime, it should be there. */
                arg[0] = (char *) "kde-open";
                arg[1] = (char *) url;
                arg[2] = NULL;

                const gboolean spawn_ret = g_spawn_sync(NULL, arg, NULL,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                                 NULL, NULL, NULL, NULL, &exitcode, NULL);

                if (spawn_ret)
                    break;
            }

            GError *error = NULL;
            if (!gtk_show_uri(/* use default screen */ NULL, url, GDK_CURRENT_TIME, &error))
                error_msg("Can't open url '%s': %s", url, error->message);

            break;
        }
    }

    if (tags)
        g_slist_free (tags);
}

/* Links can be activated by pressing Enter.
 */
static gboolean key_press_event(GtkWidget *text_view, GdkEventKey *event)
{
    GtkTextIter iter;
    GtkTextBuffer *buffer;

    switch (event->keyval)
    {
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW (text_view));
            gtk_text_buffer_get_iter_at_mark(buffer, &iter,
                    gtk_text_buffer_get_insert(buffer));
            open_browse_if_link(text_view, &iter);
            break;

        default:
            break;
    }

    return FALSE;
}

/* Links can also be activated by clicking.
 */
static gboolean event_after(GtkWidget *text_view, GdkEvent *ev)
{
    GtkTextIter start, end, iter;
    GtkTextBuffer *buffer;
    GdkEventButton *event;
    gint x, y;

    if (ev->type != GDK_BUTTON_RELEASE)
        return FALSE;

    event = (GdkEventButton *)ev;

    if (event->button != GDK_BUTTON_PRIMARY)
        return FALSE;

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));

    /* we shouldn't follow a link if the user has selected something */
    gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
    if (gtk_text_iter_get_offset(&start) != gtk_text_iter_get_offset(&end))
        return FALSE;

    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW (text_view),
                                          GTK_TEXT_WINDOW_WIDGET,
                                          event->x, event->y, &x, &y);

    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW (text_view), &iter, x, y);

    open_browse_if_link(text_view, &iter);

    return FALSE;
}

static gboolean hovering_over_link = FALSE;
static GdkCursor *hand_cursor = NULL;
static GdkCursor *regular_cursor = NULL;

/* Looks at all tags covering the position (x, y) in the text view,
 * and if one of them is a link, change the cursor to the "hands" cursor
 * typically used by web browsers.
 */
static void set_cursor_if_appropriate(GtkTextView *text_view,
                                      gint x,
                                      gint y)
{
    GSList *tags = NULL, *tagp = NULL;
    GtkTextIter iter;
    gboolean hovering = FALSE;

    gtk_text_view_get_iter_at_location(text_view, &iter, x, y);

    tags = gtk_text_iter_get_tags(&iter);
    for (tagp = tags; tagp != NULL; tagp = tagp->next)
    {
        GtkTextTag *tag = tagp->data;
        gpointer url = g_object_get_data(G_OBJECT (tag), "url");

        if (url != 0)
        {
            hovering = TRUE;
            break;
        }
    }

    if (hovering != hovering_over_link)
    {
        hovering_over_link = hovering;

        if (hovering_over_link)
            gdk_window_set_cursor(gtk_text_view_get_window(text_view, GTK_TEXT_WINDOW_TEXT), hand_cursor);
        else
            gdk_window_set_cursor(gtk_text_view_get_window(text_view, GTK_TEXT_WINDOW_TEXT), regular_cursor);
    }

    if (tags)
        g_slist_free (tags);
}


/* Update the cursor image if the pointer moved.
 */
static gboolean motion_notify_event(GtkWidget *text_view, GdkEventMotion *event)
{
    gint x, y;

    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(text_view),
                                          GTK_TEXT_WINDOW_WIDGET,
                                          event->x, event->y, &x, &y);

    set_cursor_if_appropriate(GTK_TEXT_VIEW(text_view), x, y);
    return FALSE;
}

/* Also update the cursor image if the window becomes visible
 * (e.g. when a window covering it got iconified).
 */
static gboolean visibility_notify_event(GtkWidget *text_view, GdkEventVisibility *event)
{
    gint wx, wy, bx, by;

    GdkWindow *win = gtk_text_view_get_window(GTK_TEXT_VIEW(text_view), GTK_TEXT_WINDOW_TEXT);
    GdkDeviceManager *device_manager = gdk_display_get_device_manager(gdk_window_get_display (win));
    GdkDevice *pointer = gdk_device_manager_get_client_pointer(device_manager);
    gdk_window_get_device_position(gtk_widget_get_window(text_view), pointer, &wx, &wy, NULL);

    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(text_view),
                                          GTK_TEXT_WINDOW_WIDGET,
                                          wx, wy, &bx, &by);

    set_cursor_if_appropriate(GTK_TEXT_VIEW (text_view), bx, by);

    return FALSE;
}

/* event_gui_data_t */

static event_gui_data_t *new_event_gui_data_t(void)
{
    return xzalloc(sizeof(event_gui_data_t));
}

static void free_event_gui_data_t(event_gui_data_t *evdata, void *unused)
{
    if (evdata)
    {
        free(evdata->event_name);
        free(evdata);
    }
}


/* tv_details handling */

static struct problem_item *get_current_problem_item_or_NULL(GtkTreeView *tree_view, gchar **pp_item_name)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTreeSelection* selection = gtk_tree_view_get_selection(tree_view);

    if (selection == NULL)
        return NULL;

    if (!gtk_tree_selection_get_selected(selection, &model, &iter))
        return NULL;

    *pp_item_name = NULL;
    gtk_tree_model_get(model, &iter,
                DETAIL_COLUMN_NAME, pp_item_name,
                -1);
    if (!*pp_item_name) /* paranoia, should never happen */
        return NULL;
    struct problem_item *item = problem_data_get_item_or_NULL(g_cd, *pp_item_name);
    return item;
}

static void tv_details_row_activated(
                        GtkTreeView       *tree_view,
                        GtkTreePath       *tree_path_UNUSED,
                        GtkTreeViewColumn *column,
                        gpointer           user_data)
{
    gchar *item_name;
    struct problem_item *item = get_current_problem_item_or_NULL(tree_view, &item_name);
    if (!item || !(item->flags & CD_FLAG_TXT))
        goto ret;
    if (!strchr(item->content, '\n')) /* one line? */
        goto ret; /* yes */

    gint exitcode;
    gchar *arg[3];
    arg[0] = (char *) "xdg-open";
    arg[1] = concat_path_file(g_dump_dir_name, item_name);
    arg[2] = NULL;

    const gboolean spawn_ret = g_spawn_sync(NULL, arg, NULL,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                                 NULL, NULL, NULL, NULL, &exitcode, NULL);

    if (spawn_ret == FALSE || exitcode != EXIT_SUCCESS)
    {
        GtkWidget *dialog = gtk_dialog_new_with_buttons(_("View/edit a text file"),
            GTK_WINDOW(g_wnd_assistant),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            NULL, NULL);
        GtkWidget *vbox = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        GtkWidget *textview = gtk_text_view_new();

        gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Save"), GTK_RESPONSE_OK);
        gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Cancel"), GTK_RESPONSE_CANCEL);

        gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);
        gtk_widget_set_size_request(scrolled, 640, 480);
        gtk_widget_show(scrolled);

#if ((GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 7) || (GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION == 7 && GTK_MICRO_VERSION < 8))
        /* http://developer.gnome.org/gtk3/unstable/GtkScrolledWindow.html#gtk-scrolled-window-add-with-viewport */
        /* gtk_scrolled_window_add_with_viewport has been deprecated since version 3.8 and should not be used in newly-written code. */
        gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scrolled), textview);
#else
        /* gtk_container_add() will now automatically add a GtkViewport if the child doesn't implement GtkScrollable. */
        gtk_container_add(GTK_CONTAINER(scrolled), textview);
#endif

        gtk_widget_show(textview);

        load_text_to_text_view(GTK_TEXT_VIEW(textview), item_name);

        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
        {
            save_text_from_text_view(GTK_TEXT_VIEW(textview), item_name);
            problem_data_reload_from_dump_dir();
            update_gui_state_from_problem_data(/* don't update selected event */ 0);
        }

        gtk_widget_destroy(textview);
        gtk_widget_destroy(scrolled);
        gtk_widget_destroy(dialog);
    }

    free(arg[1]);
 ret:
    g_free(item_name);
}

/* static gboolean tv_details_select_cursor_row(
                        GtkTreeView *tree_view,
                        gboolean arg1,
                        gpointer user_data) {...} */

static void tv_details_cursor_changed(
                        GtkTreeView *tree_view,
                        gpointer     user_data_UNUSED)
{
    /* I see this being called on window "destroy" signal when the tree_view is
       not a tree view anymore (or destroyed?) causing this error msg:
       (abrt:12804): Gtk-CRITICAL **: gtk_tree_selection_get_selected: assertion `GTK_IS_TREE_SELECTION (selection)' failed
       (abrt:12804): GLib-GObject-WARNING **: invalid uninstantiatable type `(null)' in cast to `GObject'
       (abrt:12804): GLib-GObject-CRITICAL **: g_object_set: assertion `G_IS_OBJECT (object)' failed
    */
    if (!GTK_IS_TREE_VIEW(tree_view))
        return;

    gchar *item_name = NULL;
    struct problem_item *item = get_current_problem_item_or_NULL(tree_view, &item_name);
    g_free(item_name);

    /* happens when closing the wizard by clicking 'X' */
    if (!item)
        return;

    gboolean editable = (item
                /* With this, copying of non-editable fields are more difficult */
                //&& (item->flags & CD_FLAG_ISEDITABLE)
                && (item->flags & CD_FLAG_TXT)
                && !strchr(item->content, '\n')
    );

    /* Allow user to select the text with mouse.
     * Has undesirable side-effect of allowing user to "edit" the text,
     * but changes aren't saved (the old text reappears as soon as user
     * leaves the field). Need to disable editing somehow.
     */
    g_object_set(G_OBJECT(g_tv_details_renderer_value),
                "editable", editable,
                NULL);
}

static void g_tv_details_checkbox_toggled(
                        GtkCellRendererToggle *cell_renderer_UNUSED,
                        gchar    *tree_path,
                        gpointer  user_data_UNUSED)
{
    //log("%s: path:'%s'", __func__, tree_path);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(g_ls_details), &iter, tree_path))
        return;

    gchar *item_name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(g_ls_details), &iter,
                DETAIL_COLUMN_NAME, &item_name,
                -1);
    if (!item_name) /* paranoia, should never happen */
        return;
    struct problem_item *item = problem_data_get_item_or_NULL(g_cd, item_name);
    g_free(item_name);
    if (!item) /* paranoia */
        return;

    int cur_value;
    if (item->selected_by_user == 0)
        cur_value = item->default_by_reporter;
    else
        cur_value = !!(item->selected_by_user + 1); /* map -1,1 to 0,1 */
    //log("%s: allowed:%d reqd:%d def:%d user:%d cur:%d", __func__,
    //            item->allowed_by_reporter,
    //            item->required_by_reporter,
    //            item->default_by_reporter,
    //            item->selected_by_user,
    //            cur_value
    //);
    if (item->allowed_by_reporter && !item->required_by_reporter)
    {
        cur_value = !cur_value;
        item->selected_by_user = cur_value * 2 - 1; /* map 0,1 to -1,1 */
        //log("%s: now ->selected_by_user=%d", __func__, item->selected_by_user);
        gtk_list_store_set(g_ls_details, &iter,
                DETAIL_COLUMN_CHECKBOX, cur_value,
                -1);
    }
}


/* update_gui_state_from_problem_data */

static gint find_by_button(gconstpointer a, gconstpointer button)
{
    const event_gui_data_t *evdata = a;
    return (evdata->toggle_button != button);
}

static void check_event_config(const char *event_name)
{
    GHashTable *errors = validate_event(event_name);
    if (errors != NULL)
    {
        g_hash_table_unref(errors);
        show_event_config_dialog(event_name, GTK_WINDOW(g_top_most_window));
    }
}

static void event_rb_was_toggled(GtkButton *button, gpointer user_data)
{
    /* Note: called both when item is selected and _unselected_,
     * use gtk_toggle_button_get_active() to determine state.
     */
    GList *found = g_list_find_custom(g_list_events, button, find_by_button);
    if (found)
    {
        event_gui_data_t *evdata = found->data;
        if (gtk_toggle_button_get_active(evdata->toggle_button))
        {
            free(g_event_selected);
            g_event_selected = xstrdup(evdata->event_name);
            check_event_config(evdata->event_name);

            clear_warnings();
            const bool good_rating = check_minimal_bt_rating(g_event_selected);
            show_warnings();

            gtk_widget_set_sensitive(g_btn_next, good_rating);
        }
    }
}

/* event_name contains "EVENT1\nEVENT2\nEVENT3\n".
 * Add new radio buttons to GtkBox for each EVENTn.
 * Remember them in GList **p_event_list (list of event_gui_data_t's).
 * Set "toggled" callback on each button to given GCallback if it's not NULL.
 * Return active button (or NULL if none created).
 */
/* helper */
static char *missing_items_in_comma_list(const char *input_item_list)
{
    if (!input_item_list)
        return NULL;

    char *item_list = xstrdup(input_item_list);
    char *result = item_list;
    char *dst = item_list;

    while (item_list[0])
    {
        char *end = strchr(item_list, ',');
        if (end) *end = '\0';
        if (!problem_data_get_item_or_NULL(g_cd, item_list))
        {
            if (dst != result)
                *dst++ = ',';
            dst = stpcpy(dst, item_list);
        }
        if (!end)
            break;
        *end = ',';
        item_list = end + 1;
    }
    if (result == dst)
    {
        free(result);
        result = NULL;
    }
    return result;
}

static event_gui_data_t *add_event_buttons(GtkBox *box,
                GList **p_event_list,
                char *event_name,
                GCallback func)
{
    //log_info("removing all buttons from box %p", box);
    gtk_container_foreach(GTK_CONTAINER(box), &remove_child_widget, NULL);
    g_list_foreach(*p_event_list, (GFunc)free_event_gui_data_t, NULL);
    g_list_free(*p_event_list);
    *p_event_list = NULL;

    g_black_event_count = 0;

    if (!event_name || !event_name[0])
    {
        GtkWidget *lbl = gtk_label_new(_("No reporting targets are defined for this problem. Check configuration in /etc/libreport/*"));
#if ((GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 13) || (GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION == 13 && GTK_MICRO_VERSION < 5))
        gtk_misc_set_alignment(GTK_MISC(lbl), /*x*/ 0.0, /*y*/ 0.0);
#else
        gtk_widget_set_halign (lbl, GTK_ALIGN_START);
        gtk_widget_set_valign (lbl, GTK_ALIGN_END);
#endif
        make_label_autowrap_on_resize(GTK_LABEL(lbl));
        gtk_box_pack_start(box, lbl, /*expand*/ true, /*fill*/ false, /*padding*/ 0);
        return NULL;
    }

    event_gui_data_t *first_button = NULL;
    event_gui_data_t *active_button = NULL;
    while (1)
    {
        if (!event_name || !event_name[0])
            break;

        char *event_name_end = strchr(event_name, '\n');
        *event_name_end = '\0';

        event_config_t *cfg = get_event_config(event_name);

        /* Form a pretty text representation of event */
        /* By default, use event name: */
        const char *event_screen_name = event_name;
        const char *event_description = NULL;
        char *tmp_description = NULL;
        bool red_choice = false;
        bool green_choice = false;
        if (cfg)
        {
            /* .xml has (presumably) prettier description, use it: */
            if (ec_get_screen_name(cfg))
                event_screen_name = ec_get_screen_name(cfg);
            event_description = ec_get_description(cfg);

            char *missing = missing_items_in_comma_list(cfg->ec_requires_items);
            if (missing)
            {
                red_choice = true;
                event_description = tmp_description = xasprintf(_("(requires: %s)"), missing);
                free(missing);
            }
            else
            if (cfg->ec_creates_items)
            {
                if (problem_data_get_item_or_NULL(g_cd, cfg->ec_creates_items))
                {
                    char *missing = missing_items_in_comma_list(cfg->ec_creates_items);
                    if (missing)
                        free(missing);
                    else
                    {
                        green_choice = true;
                        event_description = tmp_description = xasprintf(_("(not needed, data already exist: %s)"), cfg->ec_creates_items);
                    }
                }
            }
        }
        if (!green_choice && !red_choice)
            g_black_event_count++;

        //log_info("adding button '%s' to box %p", event_name, box);
        char *event_label = xasprintf("%s%s%s",
                        event_screen_name,
                        (event_description ? " - " : ""),
                        event_description ? event_description : ""
        );
        free(tmp_description);

        GtkWidget *button = gtk_radio_button_new_with_label_from_widget(
                        (first_button ? GTK_RADIO_BUTTON(first_button->toggle_button) : NULL),
                        event_label
                );
        free(event_label);

        if (green_choice || red_choice)
        {
            GtkWidget *child = gtk_bin_get_child(GTK_BIN(button));
            if (child)
            {
                static const GdkRGBA red = {
                    .red   = 1.0,
                    .green = 0.0,
                    .blue  = 0.0,
                    .alpha = 1.0,
                };
                static const GdkRGBA green = {
                    .red   = 0.0,
                    .green = 0.5,
                    .blue  = 0.0,
                    .alpha = 1.0,
                };
                const GdkRGBA *color = (green_choice ? &green : &red);
                //gtk_widget_modify_text(button, GTK_STATE_NORMAL, color);
                gtk_widget_override_color(child, GTK_STATE_FLAG_NORMAL, color);
            }
        }

        if (func)
            g_signal_connect(G_OBJECT(button), "toggled", func, xstrdup(event_name));

        if (cfg && ec_get_long_desc(cfg))
            gtk_widget_set_tooltip_text(button, ec_get_long_desc(cfg));

        event_gui_data_t *event_gui_data = new_event_gui_data_t();
        event_gui_data->event_name = xstrdup(event_name);
        event_gui_data->toggle_button = GTK_TOGGLE_BUTTON(button);
        *p_event_list = g_list_append(*p_event_list, event_gui_data);

        if (!first_button)
            first_button = event_gui_data;

        if (!green_choice && !red_choice && !active_button)
        {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), true);
            active_button = event_gui_data;
        }

        *event_name_end = '\n';
        event_name = event_name_end + 1;

        gtk_box_pack_start(box, button, /*expand*/ false, /*fill*/ false, /*padding*/ 0);
        gtk_widget_show_all(GTK_WIDGET(button));
        wrap_all_labels(button);
        /* Disabled - seems that above is enough... */
        /*fix_all_wrapped_labels(button);*/
    }
    gtk_widget_show_all(GTK_WIDGET(box));

    return active_button;
}

struct cd_stats {
    off_t filesize;
    unsigned filecount;
};

static void save_items_from_notepad(void)
{
    gint n_pages = gtk_notebook_get_n_pages(g_notebook);
    int i = 0;

    GtkWidget *notebook_child;
    GtkTextView *tev;
    GtkWidget *tab_lbl;
    const char *item_name;

    for (i = 0; i < n_pages; i++)
    {
        //notebook_page->scrolled_window->text_view
        notebook_child = gtk_notebook_get_nth_page(g_notebook, i);
        tev = GTK_TEXT_VIEW(gtk_bin_get_child(GTK_BIN(notebook_child)));
        tab_lbl = gtk_notebook_get_tab_label(g_notebook, notebook_child);
        item_name = gtk_label_get_text(GTK_LABEL(tab_lbl));
        log_notice("saving: '%s'", item_name);

        save_text_from_text_view(tev, item_name);
    }
}

static void remove_tabs_from_notebook(GtkNotebook *notebook)
{
    gint n_pages = gtk_notebook_get_n_pages(notebook);
    int ii;

    for (ii = 0; ii < n_pages; ii++)
    {
        /* removing a page changes the indices, so we always need to remove
         * page 0
        */
        gtk_notebook_remove_page(notebook, 0); //we need to always the page 0
    }

    /* Turn off the changed callback during the update */
    g_signal_handler_block(g_tv_sensitive_sel, g_tv_sensitive_sel_hndlr);

    g_current_highlighted_word = NULL;

    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_ls_sensitive_list), &iter);
    while (valid)
    {
        char *text = NULL;
        search_item_t *word = NULL;

        gtk_tree_model_get(GTK_TREE_MODEL(g_ls_sensitive_list), &iter,
                SEARCH_COLUMN_TEXT, &text,
                SEARCH_COLUMN_ITEM, &word,
                -1);

        free(text);
        free(word);

        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_ls_sensitive_list), &iter);
    }
    gtk_list_store_clear(g_ls_sensitive_list);
    g_signal_handler_unblock(g_tv_sensitive_sel, g_tv_sensitive_sel_hndlr);
}

static void append_item_to_ls_details(gpointer name, gpointer value, gpointer data)
{
    problem_item *item = (problem_item*)value;
    struct cd_stats *stats = data;
    GtkTreeIter iter;

    gtk_list_store_append(g_ls_details, &iter);
    stats->filecount++;

    //FIXME: use the human-readable problem_item_format(item) instead of item->content.
    if (item->flags & CD_FLAG_TXT)
    {
        if (item->flags & CD_FLAG_ISEDITABLE && strcmp(name, FILENAME_ANACONDA_TB) != 0)
        {
            GtkWidget *tab_lbl = gtk_label_new((char *)name);
            GtkWidget *tev = gtk_text_view_new();

            if (strcmp(name, FILENAME_COMMENT) == 0 || strcmp(name, FILENAME_REASON) == 0)
                gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tev), GTK_WRAP_WORD);

            gtk_widget_override_font(GTK_WIDGET(tev), g_monospace_font);
            load_text_to_text_view(GTK_TEXT_VIEW(tev), (char *)name);
            /* init searching */
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tev));
            /* found items background */
            gtk_text_buffer_create_tag(buf, "search_result_bg", "background", "red", NULL);
            gtk_text_buffer_create_tag(buf, "current_result_bg", "background", "green", NULL);
            GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
            gtk_container_add(GTK_CONTAINER(sw), tev);
            gtk_notebook_append_page(g_notebook, sw, tab_lbl);
        }
        stats->filesize += strlen(item->content);
        /* If not multiline... */
        if (!strchr(item->content, '\n'))
        {
            gtk_list_store_set(g_ls_details, &iter,
                              DETAIL_COLUMN_NAME, (char *)name,
                              DETAIL_COLUMN_VALUE, item->content,
                              -1);
        }
        else
        {
            gtk_list_store_set(g_ls_details, &iter,
                              DETAIL_COLUMN_NAME, (char *)name,
                              DETAIL_COLUMN_VALUE, _("(click here to view/edit)"),
                              -1);
        }
    }
    else if (item->flags & CD_FLAG_BIN)
    {
        struct stat statbuf;
        statbuf.st_size = 0;
        if (stat(item->content, &statbuf) == 0)
        {
            stats->filesize += statbuf.st_size;
            char *msg = xasprintf(_("(binary file, %llu bytes)"), (long long)statbuf.st_size);
            gtk_list_store_set(g_ls_details, &iter,
                                  DETAIL_COLUMN_NAME, (char *)name,
                                  DETAIL_COLUMN_VALUE, msg,
                                  -1);
            free(msg);
        }
    }

    int cur_value;
    if (item->selected_by_user == 0)
        cur_value = item->default_by_reporter;
    else
        cur_value = !!(item->selected_by_user + 1); /* map -1,1 to 0,1 */

    gtk_list_store_set(g_ls_details, &iter,
            DETAIL_COLUMN_CHECKBOX, cur_value,
            -1);
}

/* Based on selected reporter, update item checkboxes */
static void update_ls_details_checkboxes(const char *event_name)
{
    event_config_t *cfg = get_event_config(event_name);
    //log("%s: event:'%s', cfg:'%p'", __func__, g_event_selected, cfg);
    GHashTableIter iter;
    char *name;
    struct problem_item *item;
    g_hash_table_iter_init(&iter, g_cd);
    string_vector_ptr_t global_exclude = get_global_always_excluded_elements();
    while (g_hash_table_iter_next(&iter, (void**)&name, (void**)&item))
    {
        /* Decide whether item is allowed, required, and what's the default */
        item->allowed_by_reporter = 1;
        if (global_exclude)
            item->allowed_by_reporter = !is_in_string_list(name, (const_string_vector_const_ptr_t)global_exclude);

        if (cfg)
        {
            if (is_in_comma_separated_list_of_glob_patterns(name, cfg->ec_exclude_items_always))
                item->allowed_by_reporter = 0;
            if ((item->flags & CD_FLAG_BIN) && cfg->ec_exclude_binary_items)
                item->allowed_by_reporter = 0;
        }

        item->default_by_reporter = item->allowed_by_reporter;
        if (cfg)
        {
            if (is_in_comma_separated_list_of_glob_patterns(name, cfg->ec_exclude_items_by_default))
                item->default_by_reporter = 0;
            if (is_in_comma_separated_list_of_glob_patterns(name, cfg->ec_include_items_by_default))
                item->allowed_by_reporter = item->default_by_reporter = 1;
        }

        item->required_by_reporter = 0;
        if (cfg)
        {
            if (is_in_comma_separated_list_of_glob_patterns(name, cfg->ec_requires_items))
                item->default_by_reporter = item->allowed_by_reporter = item->required_by_reporter = 1;
        }

        int cur_value;
        if (item->selected_by_user == 0)
            cur_value = item->default_by_reporter;
        else
            cur_value = !!(item->selected_by_user + 1); /* map -1,1 to 0,1 */

        //log("%s: '%s' allowed:%d reqd:%d def:%d user:%d", __func__, name,
        //    item->allowed_by_reporter,
        //    item->required_by_reporter,
        //    item->default_by_reporter,
        //    item->selected_by_user
        //);

        /* Find corresponding line and update checkbox */
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_ls_details), &iter))
        {
            do {
                gchar *item_name = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(g_ls_details), &iter,
                            DETAIL_COLUMN_NAME, &item_name,
                            -1);
                if (!item_name) /* paranoia, should never happen */
                    continue;
                int differ = strcmp(name, item_name);
                g_free(item_name);
                if (differ)
                    continue;
                gtk_list_store_set(g_ls_details, &iter,
                        DETAIL_COLUMN_CHECKBOX, cur_value,
                        -1);
                //log("%s: changed gtk_list_store_set to %d", __func__, (item->allowed_by_reporter && item->selected_by_user >= 0));
                break;
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(g_ls_details), &iter));
        }
    }
}

void update_gui_state_from_problem_data(int flags)
{
    update_window_title();
    remove_tabs_from_notebook(g_notebook);

    const char *reason = problem_data_get_content_or_NULL(g_cd, FILENAME_REASON);
    const char *not_reportable = problem_data_get_content_or_NULL(g_cd,
                                                                  FILENAME_NOT_REPORTABLE);

    char *t = xasprintf("%s%s%s",
                        not_reportable ? : "",
                        not_reportable ? " " : "",
                        reason ? : _("(no description)"));

    gtk_label_set_text(g_lbl_cd_reason, t);
    free(t);

    gtk_list_store_clear(g_ls_details);
    struct cd_stats stats = { 0 };
    g_hash_table_foreach(g_cd, append_item_to_ls_details, &stats);
    char *msg = xasprintf(_("%llu bytes, %u files"), (long long)stats.filesize, stats.filecount);
    gtk_label_set_text(g_lbl_size, msg);
    free(msg);

    load_text_to_text_view(g_tv_comment, FILENAME_COMMENT);

    add_workflow_buttons(g_box_workflows, g_workflow_list,
                        G_CALLBACK(set_auto_event_chain));

    /* Update event radio buttons
     * show them only in expert mode
    */
    event_gui_data_t *active_button = NULL;
    if (g_expert_mode == true)
    {
        //this widget doesn't react to show_all, so we need to "force" it
        gtk_widget_show(GTK_WIDGET(g_box_events));
        active_button = add_event_buttons(
                    g_box_events,
                    &g_list_events,
                    g_events,
                    G_CALLBACK(event_rb_was_toggled)
        );
    }

    if (flags & UPDATE_SELECTED_EVENT && g_expert_mode)
    {
        /* Update the value of currently selected event */
        free(g_event_selected);
        g_event_selected = NULL;
        if (active_button)
        {
            g_event_selected = xstrdup(active_button->event_name);
        }
        log_info("g_event_selected='%s'", g_event_selected);
    }
    /* We can't just do gtk_widget_show_all once in main:
     * We created new widgets (buttons). Need to make them visible.
     */
    gtk_widget_show_all(GTK_WIDGET(g_wnd_assistant));
}


/* start_event_run */

struct analyze_event_data
{
    struct run_event_state *run_state;
    char *event_name;
    GList *env_list;
    GIOChannel *channel;
    struct strbuf *event_log;
    int event_log_state;
    int fd;
    /*guint event_source_id;*/
};
enum {
    LOGSTATE_FIRSTLINE = 0,
    LOGSTATE_BEGLINE,
    LOGSTATE_ERRLINE,
    LOGSTATE_MIDLINE,
};

static void set_excluded_envvar(void)
{
    struct strbuf *item_list = strbuf_new();
    const char *fmt = "%s";

    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_ls_details), &iter))
    {
        do {
            gchar *item_name = NULL;
            gboolean checked = 0;
            gtk_tree_model_get(GTK_TREE_MODEL(g_ls_details), &iter,
                    DETAIL_COLUMN_NAME, &item_name,
                    DETAIL_COLUMN_CHECKBOX, &checked,
                    -1);
            if (!item_name) /* paranoia, should never happen */
                continue;
            if (!checked)
            {
                strbuf_append_strf(item_list, fmt, item_name);
                fmt = ",%s";
            }
            g_free(item_name);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(g_ls_details), &iter));
    }
    char *var = strbuf_free_nobuf(item_list);
    //log("EXCLUDE_FROM_REPORT='%s'", var);
    if (var)
    {
        xsetenv("EXCLUDE_FROM_REPORT", var);
        free(var);
    }
    else
        unsetenv("EXCLUDE_FROM_REPORT");
}

static int spawn_next_command_in_evd(struct analyze_event_data *evd)
{
    evd->env_list = export_event_config(evd->event_name);
    int r = spawn_next_command(evd->run_state, g_dump_dir_name, evd->event_name, EXECFLG_SETPGID);
    if (r >= 0)
    {
        g_event_child_pid = evd->run_state->command_pid;
    }
    else
    {
        unexport_event_config(evd->env_list);
        evd->env_list = NULL;
    }
    return r;
}

static void save_to_event_log(struct analyze_event_data *evd, const char *str)
{
    static const char delim[] = {
        [LOGSTATE_FIRSTLINE] = '>',
        [LOGSTATE_BEGLINE] = ' ',
        [LOGSTATE_ERRLINE] = '*',
    };

    while (str[0])
    {
        char *end = strchrnul(str, '\n');
        char end_char = *end;
        if (end_char == '\n')
            end++;
        switch (evd->event_log_state)
        {
            case LOGSTATE_FIRSTLINE:
            case LOGSTATE_BEGLINE:
            case LOGSTATE_ERRLINE:
                /* skip empty lines */
                if (str[0] == '\n')
                    goto next;
                strbuf_append_strf(evd->event_log, "%s%c %.*s",
                        iso_date_string(NULL),
                        delim[evd->event_log_state],
                        (int)(end - str), str
                );
                break;
            case LOGSTATE_MIDLINE:
                strbuf_append_strf(evd->event_log, "%.*s", (int)(end - str), str);
                break;
        }
        evd->event_log_state = LOGSTATE_MIDLINE;
        if (end_char != '\n')
            break;
        evd->event_log_state = LOGSTATE_BEGLINE;
 next:
        str = end;
    }
}

static void update_event_log_on_disk(const char *str)
{
    /* Load existing log */
    struct dump_dir *dd = dd_opendir(g_dump_dir_name, 0);
    if (!dd)
        return;
    char *event_log = dd_load_text_ext(dd, FILENAME_EVENT_LOG, DD_FAIL_QUIETLY_ENOENT);

    /* Append new log part to existing log */
    unsigned len = strlen(event_log);
    if (len != 0 && event_log[len - 1] != '\n')
        event_log = append_to_malloced_string(event_log, "\n");
    event_log = append_to_malloced_string(event_log, str);

    /* Trim log according to size watermarks */
    len = strlen(event_log);
    char *new_log = event_log;
    if (len > EVENT_LOG_HIGH_WATERMARK)
    {
        new_log += len - EVENT_LOG_LOW_WATERMARK;
        new_log = strchrnul(new_log, '\n');
        if (new_log[0])
            new_log++;
    }

    /* Save */
    dd_save_text(dd, FILENAME_EVENT_LOG, new_log);
    free(event_log);
    dd_close(dd);
}

static bool cancel_event_run()
{
    if (g_event_child_pid <= 0)
        return false;

    kill(- g_event_child_pid, SIGTERM);
    return true;
}

static void on_btn_cancel_event(GtkButton *button)
{
    cancel_event_run();
}

static bool is_processing_finished()
{
    return !g_expert_mode && !g_auto_event_list;
}

static void hide_next_step_button()
{
    /* replace 'Forward' with 'Close' button */
    /* 1. hide next button */
    gtk_widget_hide(g_btn_next);
    /* 2. move close button to the last position */
    gtk_box_set_child_packing(g_box_buttons, g_btn_close, false, false, 5, GTK_PACK_END);
}

static void show_next_step_button()
{
    gtk_box_set_child_packing(g_box_buttons, g_btn_close, false, false, 5, GTK_PACK_START);

    gtk_widget_show(g_btn_next);
}

enum {
 TERMINATE_NOFLAGS    = 0,
 TERMINATE_WITH_RERUN = 1 << 0,
};

static void terminate_event_chain(int flags)
{
    if (g_expert_mode)
        return;

    hide_next_step_button();
    if ((flags & TERMINATE_WITH_RERUN))
        return;

    free(g_event_selected);
    g_event_selected = NULL;

    g_list_free_full(g_auto_event_list, free);
    g_auto_event_list = NULL;
}

static void cancel_processing(GtkLabel *status_label, const char *message, int terminate_flags)
{
    gtk_label_set_text(status_label, message ? message : _("Processing was canceled"));
    terminate_event_chain(terminate_flags);
}

static void update_command_run_log(const char* message, struct analyze_event_data *evd)
{
    const bool it_is_a_dot = (message[0] == '.' && message[1] == '\0');

    if (!it_is_a_dot)
        gtk_label_set_text(g_lbl_event_log, message);

    /* Don't append new line behind single dot */
    const char *log_msg = it_is_a_dot ? message : xasprintf("%s\n", message);
    append_to_textview(g_tv_event_log, log_msg);
    save_to_event_log(evd, log_msg);

    /* Because of single dot, see lines above */
    if (log_msg != message)
        free((void *)log_msg);
}

static void run_event_gtk_error(const char *error_line, void *param)
{
    update_command_run_log(error_line, (struct analyze_event_data *)param);
}

static char *run_event_gtk_logging(char *log_line, void *param)
{
    struct analyze_event_data *evd = (struct analyze_event_data *)param;
    update_command_run_log(log_line, evd);
    return log_line;
}

static void log_request_response_communication(const char *request, const char *response, struct analyze_event_data *evd)
{
    char *message = xasprintf(response ? "%s '%s'" : "%s", request, response);
    update_command_run_log(message, evd);
    free(message);
}

static void run_event_gtk_alert(const char *msg, void *args)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_wnd_assistant),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_CLOSE,
            "%s", msg);
    char *tagged_msg = tag_url(msg, "\n");
    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog), tagged_msg);
    free(tagged_msg);

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    log_request_response_communication(msg, NULL, (struct analyze_event_data *)args);
}

static void gtk_entry_emit_dialog_response_ok(GtkEntry *entry, GtkDialog *dialog)
{
    /* Don't close the dialogue if the entry is empty */
    if (gtk_entry_get_text_length(entry) > 0)
        gtk_dialog_response(dialog, GTK_RESPONSE_OK);
}

static char *ask_helper(const char *msg, void *args, bool password)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_wnd_assistant),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_OK_CANCEL,
            "%s", msg);
    char *tagged_msg = tag_url(msg, "\n");
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog), tagged_msg);
    free(tagged_msg);

    GtkWidget *vbox = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *textbox = gtk_entry_new();
    /* gtk_entry_set_editable(GTK_ENTRY(textbox), TRUE);
     * is not available in gtk3, so please use the highlevel
     * g_object_set
     */
    g_object_set(G_OBJECT(textbox), "editable", TRUE, NULL);
    g_signal_connect(textbox, "activate", G_CALLBACK(gtk_entry_emit_dialog_response_ok), dialog);

    if (password)
        gtk_entry_set_visibility(GTK_ENTRY(textbox), FALSE);

    gtk_box_pack_start(GTK_BOX(vbox), textbox, TRUE, TRUE, 0);
    gtk_widget_show(textbox);

    char *response = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
    {
        const char *text = gtk_entry_get_text(GTK_ENTRY(textbox));
        response = xstrdup(text);
    }

    gtk_widget_destroy(textbox);
    gtk_widget_destroy(dialog);

    const char *log_response = "";
    if (response)
        log_response = password ? "********" : response;

    log_request_response_communication(msg, log_response, (struct analyze_event_data *)args);
    return response ? response : xstrdup("");
}

static char *run_event_gtk_ask(const char *msg, void *args)
{
    return ask_helper(msg, args, false);
}

static int run_event_gtk_ask_yes_no(const char *msg, void *args)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_wnd_assistant),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_YES_NO,
            "%s", msg);
    char *tagged_msg = tag_url(msg, "\n");
    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog), tagged_msg);
    free(tagged_msg);

    /* Esc -> No, Enter -> Yes */
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    const int ret = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES;

    gtk_widget_destroy(dialog);

    log_request_response_communication(msg, ret ? "YES" : "NO", (struct analyze_event_data *)args);
    return ret;
}

static int run_event_gtk_ask_yes_no_yesforever(const char *key, const char *msg, void *args)
{
    const int ret = run_ask_yes_no_yesforever_dialog(key, msg, GTK_WINDOW(g_wnd_assistant));
    log_request_response_communication(msg, ret ? "YES" : "NO", (struct analyze_event_data *)args);
    return ret;
}

static int run_event_gtk_ask_yes_no_save_result(const char *key, const char *msg, void *args)
{
    const int ret = run_ask_yes_no_save_result_dialog(key, msg, GTK_WINDOW(g_wnd_assistant));
    log_request_response_communication(msg, ret ? "YES" : "NO", (struct analyze_event_data *)args);
    return ret;
}

static char *run_event_gtk_ask_password(const char *msg, void *args)
{
    return ask_helper(msg, args, true);
}

static bool event_need_review(const char *event_name)
{
    event_config_t *event_cfg = get_event_config(event_name);
    return !event_cfg || !event_cfg->ec_skip_review;
}

static void on_btn_failed_cb(GtkButton *button)
{
    /* Since the Repeat button has been introduced, the event chain isn't
     * terminated upon a failure in order to be able to continue in processing
     * in the retry action.
     *
     * Now, user decided to run the emergency analysis instead of trying to
     * reconfigure libreport, so we have to terminate the event chain.
     */
    gtk_widget_hide(g_btn_repeat);
    terminate_event_chain(TERMINATE_NOFLAGS);

    /* Show detailed log */
    gtk_expander_set_expanded(g_exp_report_log, TRUE);

    clear_warnings();
    update_ls_details_checkboxes(EMERGENCY_ANALYSIS_EVENT_NAME);
    start_event_run(EMERGENCY_ANALYSIS_EVENT_NAME);

    /* single shot button -> hide after click */
    gtk_widget_hide(GTK_WIDGET(button));
}

static gint select_next_page_no(gint current_page_no, gpointer data);
static void on_page_prepare(GtkNotebook *assistant, GtkWidget *page, gpointer user_data);

static void on_btn_repeat_cb(GtkButton *button)
{
    g_auto_event_list = g_list_prepend(g_auto_event_list, g_event_selected);
    g_event_selected = NULL;

    show_next_step_button();
    clear_warnings();

    const gint current_page_no = gtk_notebook_get_current_page(g_assistant);
    const int next_page_no = select_next_page_no(pages[PAGENO_SUMMARY].page_no, NULL);
    if (current_page_no == next_page_no)
        on_page_prepare(g_assistant, gtk_notebook_get_nth_page(g_assistant, next_page_no), NULL);
    else
        gtk_notebook_set_current_page(g_assistant, next_page_no);
}

static void on_failed_event(const char *event_name)
{
    /* Don't show the 'on failure' button if the processed event
     * was started by that button. (avoid infinite loop)
     */
    if (strcmp(event_name, EMERGENCY_ANALYSIS_EVENT_NAME) == 0)
        return;

   add_warning(
_("Processing of the problem failed. This can have many reasons but there are three most common:\n"\
"\t▫ <b>network connection problems</b>\n"\
"\t▫ <b>corrupted problem data</b>\n"\
"\t▫ <b>invalid configuration</b>"
));

    if (!g_expert_mode)
    {
        add_warning(
_("If you want to update the configuration and try to report again, please open <b>Preferences</b> item\n"
"in the application menu and after applying the configuration changes click <b>Repeat</b> button."));
        gtk_widget_show(g_btn_repeat);
    }

    add_warning(
_("If you are sure that this problem is not caused by network problems neither by invalid configuration\n"
"and want to help us, please click on the upload button and provide all problem data for a deep analysis.\n"\
"<i>Before you do that, please consider the security risks. Problem data may contain sensitive information like\n"\
"passwords. The uploaded data are stored in a protected storage and only a limited number of persons can read them.</i>"));

    show_warnings();

    gtk_widget_show(g_btn_onfail);
}

static gboolean consume_cmd_output(GIOChannel *source, GIOCondition condition, gpointer data)
{
    struct analyze_event_data *evd = data;
    struct run_event_state *run_state = evd->run_state;

    bool stop_requested = false;
    int retval = consume_event_command_output(run_state, g_dump_dir_name);

    if (retval < 0 && errno == EAGAIN)
        /* We got all buffered data, but fd is still open. Done for now */
        return TRUE; /* "please don't remove this event (yet)" */

    /* EOF/error */

    if (WIFEXITED(run_state->process_status)
     && WEXITSTATUS(run_state->process_status) == EXIT_STOP_EVENT_RUN
    ) {
        retval = 0;
        run_state->process_status = 0;
        stop_requested = true;
        terminate_event_chain(TERMINATE_NOFLAGS);
    }

    unexport_event_config(evd->env_list);
    evd->env_list = NULL;

    /* Make sure "Cancel" button won't send anything (process is gone) */
    g_event_child_pid = -1;
    evd->run_state->command_pid = -1; /* just for consistency */

    /* Write a final message to the log */
    if (evd->event_log->len != 0 && evd->event_log->buf[evd->event_log->len - 1] != '\n')
        save_to_event_log(evd, "\n");

    /* If program failed, or if it finished successfully without saying anything... */
    if (retval != 0 || evd->event_log_state == LOGSTATE_FIRSTLINE)
    {
        char *msg = exit_status_as_string(evd->event_name, run_state->process_status);
        if (retval != 0)
        {
            /* If program failed, emit *error* line */
            evd->event_log_state = LOGSTATE_ERRLINE;
        }
        append_to_textview(g_tv_event_log, msg);
        save_to_event_log(evd, msg);
        free(msg);
    }

    /* Append log to FILENAME_EVENT_LOG */
    update_event_log_on_disk(evd->event_log->buf);
    strbuf_clear(evd->event_log);
    evd->event_log_state = LOGSTATE_FIRSTLINE;

    struct dump_dir *dd = NULL;
    if (geteuid() == 0)
    {
        /* Reset mode/uig/gid to correct values for all files created by event run */
        dd = dd_opendir(g_dump_dir_name, 0);
        if (dd)
            dd_sanitize_mode_and_owner(dd);
    }

    if (retval == 0 && !g_expert_mode)
    {
        /* Check whether NOT_REPORTABLE element appeared. If it did, we'll stop
         * even if exit code is "success".
         */
        if (!dd) /* why? because dd may be already open by the code above */
            dd = dd_opendir(g_dump_dir_name, DD_OPEN_READONLY | DD_FAIL_QUIETLY_EACCES);
        if (!dd)
            xfunc_die();
        char *not_reportable = dd_load_text_ext(dd, FILENAME_NOT_REPORTABLE, 0
                                            | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE
                                            | DD_FAIL_QUIETLY_ENOENT
                                            | DD_FAIL_QUIETLY_EACCES);
        if (not_reportable)
            retval = 256;
        free(not_reportable);
    }
    if (dd)
        dd_close(dd);

    /* Stop if exit code is not 0, or no more commands */
    if (stop_requested
     || retval != 0
     || spawn_next_command_in_evd(evd) < 0
    ) {
        log_notice("done running event on '%s': %d", g_dump_dir_name, retval);
        append_to_textview(g_tv_event_log, "\n");

        /* Free child output buffer */
        strbuf_free(cmd_output);
        cmd_output = NULL;

        /* Hide spinner and stop btn */
        gtk_widget_hide(GTK_WIDGET(g_spinner_event_log));
        gtk_widget_hide(g_btn_stop);
        /* Enable (un-gray out) navigation buttons */
        gtk_widget_set_sensitive(g_btn_close, true);
        gtk_widget_set_sensitive(g_btn_next, true);

        problem_data_reload_from_dump_dir();
        update_gui_state_from_problem_data(UPDATE_SELECTED_EVENT);

        if (retval != 0)
        {
            gtk_widget_show(GTK_WIDGET(g_img_process_fail));
            /* 256 means NOT_REPORTABLE */
            if (retval == 256)
                cancel_processing(g_lbl_event_log, _("Processing was interrupted because the problem is not reportable."), TERMINATE_NOFLAGS);
            else
            {
                /* We use SIGTERM to stop event processing on user's request.
                 * So SIGTERM is not a failure.
                 */
                if (retval == EXIT_CANCEL_BY_USER || WTERMSIG(run_state->process_status) == SIGTERM)
                    cancel_processing(g_lbl_event_log, /* default message */ NULL, TERMINATE_NOFLAGS);
                else
                {
                    cancel_processing(g_lbl_event_log, _("Processing failed."), TERMINATE_WITH_RERUN);
                    on_failed_event(evd->event_name);
                }
            }
        }
        else
        {
            gtk_label_set_text(g_lbl_event_log, is_processing_finished() ? _("Processing finished.")
                                                                         : _("Processing finished, please proceed to the next step."));
        }

        g_source_remove(g_event_source_id);
        g_event_source_id = 0;
        close(evd->fd);
        g_io_channel_unref(evd->channel);
        free_run_event_state(evd->run_state);
        strbuf_free(evd->event_log);
        free(evd->event_name);
        free(evd);

        /* Inform abrt-gui that it is a good idea to rescan the directory */
        kill(getppid(), SIGCHLD);

        if (is_processing_finished())
            hide_next_step_button();
        else if (retval == 0 && !g_verbose && !g_expert_mode)
            on_next_btn_cb(GTK_WIDGET(g_btn_next), NULL);

        return FALSE; /* "please remove this event" */
    }

    /* New command was started. Continue waiting for input */

    /* Transplant cmd's output fd onto old one, so that main loop
     * is none the wiser that fd it waits on has changed
     */
    xmove_fd(evd->run_state->command_out_fd, evd->fd);
    evd->run_state->command_out_fd = evd->fd; /* just to keep it consistent */
    ndelay_on(evd->fd);

    /* Revive "Cancel" button */
    g_event_child_pid = evd->run_state->command_pid;

    return TRUE; /* "please don't remove this event (yet)" */
}

static int ask_replace_old_private_group_name(void)
{
    char *message = xasprintf(_("Private ticket is requested but the group name 'private' has been deprecated. "
                                "We kindly ask you to use 'fedora_contrib_private' group name. "
                                "Click Yes button or update the configuration manually. Or click No button, if you really want to use 'private' group.\n\n"
                                "If you are not sure what this dialogue means, please trust us and click Yes button.\n\n"
                                "Read more about the private bug reports at:\n"
                                "https://github.com/abrt/abrt/wiki/FAQ#creating-private-bugzilla-tickets\n"
                                "https://bugzilla.redhat.com/show_bug.cgi?id=1044653\n"));

    char *markup_message = xasprintf(_("Private ticket is requested but the group name <i>private</i> has been deprecated. "
                                "We kindly ask you to use <i>fedora_contrib_private</i> group name. "
                                "Click Yes button or update the configuration manually. Or click No button, if you really want to use <i>private</i> group.\n\n"
                                "If you are not sure what this dialogue means, please trust us and click Yes button.\n\n"
                                "Read more about the private bug reports at:\n"
                                "<a href=\"https://github.com/abrt/abrt/wiki/FAQ#creating-private-bugzilla-tickets\">"
                                "https://github.com/abrt/abrt/wiki/FAQ#creating-private-bugzilla-tickets</a>\n"
                                "<a href=\"https://bugzilla.redhat.com/show_bug.cgi?id=1044653\">https://bugzilla.redhat.com/show_bug.cgi?id=1044653</a>\n"));

    GtkWidget *old_private_group = gtk_message_dialog_new(GTK_WINDOW(g_wnd_assistant),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        message);

    gtk_window_set_transient_for(GTK_WINDOW(old_private_group), GTK_WINDOW(g_wnd_assistant));
    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(old_private_group),
                                    markup_message);
    free(message);
    free(markup_message);

    /* Esc -> No, Enter -> Yes */
    gtk_dialog_set_default_response(GTK_DIALOG(old_private_group), GTK_RESPONSE_YES);

    gint result = gtk_dialog_run(GTK_DIALOG(old_private_group));
    gtk_widget_destroy(old_private_group);

    return result == GTK_RESPONSE_YES;
}

/*
 * https://bugzilla.redhat.com/show_bug.cgi?id=1044653
 */
static void correct_bz_private_goup_name(const char *event_name)
{
    if (strcmp("report_Bugzilla", event_name) == 0 &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gtk_builder_get_object(g_builder, PRIVATE_TICKET_CB))))
    {
        event_config_t *cfg = get_event_config(event_name);
        if (NULL != cfg)
        {
            GList *item = cfg->options;
            for ( ; item != NULL; item = g_list_next(item))
            {
                event_option_t *opt = item->data;
                if (strcmp("Bugzilla_PrivateGroups", opt->eo_name) == 0
                    && opt->eo_value
                    && strcmp(opt->eo_value, "private") == 0
                    && ask_replace_old_private_group_name())
                {
                    free(opt->eo_value);
                    opt->eo_value = xstrdup("fedora_contrib_private");
                }
            }
        }
    }
}

static void start_event_run(const char *event_name)
{
    /* Start event asynchronously on the dump dir
     * (synchronous run would freeze GUI until completion)
     */

    /* https://bugzilla.redhat.com/show_bug.cgi?id=1044653 */
    correct_bz_private_goup_name(event_name);

    struct run_event_state *state = new_run_event_state();
    state->logging_callback = run_event_gtk_logging;
    state->error_callback = run_event_gtk_error;
    state->alert_callback = run_event_gtk_alert;
    state->ask_callback = run_event_gtk_ask;
    state->ask_yes_no_callback = run_event_gtk_ask_yes_no;
    state->ask_yes_no_yesforever_callback = run_event_gtk_ask_yes_no_yesforever;
    state->ask_yes_no_save_result_callback = run_event_gtk_ask_yes_no_save_result;
    state->ask_password_callback = run_event_gtk_ask_password;

    if (prepare_commands(state, g_dump_dir_name, event_name) == 0)
    {
 no_cmds:
        /* No commands needed?! (This is untypical) */
        free_run_event_state(state);
//TODO: better msg?
        char *msg = xasprintf(_("No processing for event '%s' is defined"), event_name);
        append_to_textview(g_tv_event_log, msg);
        free(msg);
        cancel_processing(g_lbl_event_log, _("Processing failed."), TERMINATE_NOFLAGS);
        return;
    }

    struct dump_dir *dd = wizard_open_directory_for_writing(g_dump_dir_name);
    dd_close(dd);
    if (!dd)
    {
        free_run_event_state(state);
        if (!g_expert_mode)
        {
            cancel_processing(g_lbl_event_log, _("Processing interrupted: can't continue without writable directory."), TERMINATE_NOFLAGS);
        }
        return; /* user refused to steal, or write error, etc... */
    }

    set_excluded_envvar();
    GList *env_list = export_event_config(event_name);

    if (spawn_next_command(state, g_dump_dir_name, event_name, EXECFLG_SETPGID) < 0)
    {
        unexport_event_config(env_list);
        goto no_cmds;
    }
    g_event_child_pid = state->command_pid;

    /* At least one command is needed, and we started first one.
     * Hook its output fd to the main loop.
     */
    struct analyze_event_data *evd = xzalloc(sizeof(*evd));
    evd->run_state = state;
    evd->event_name = xstrdup(event_name);
    evd->env_list = env_list;
    evd->event_log = strbuf_new();
    evd->fd = state->command_out_fd;

    state->logging_param = evd;
    state->error_param = evd;
    state->interaction_param = evd;

    ndelay_on(evd->fd);
    evd->channel = g_io_channel_unix_new(evd->fd);
    g_event_source_id = g_io_add_watch(evd->channel,
            G_IO_IN | G_IO_ERR | G_IO_HUP, /* need HUP to detect EOF w/o any data */
            consume_cmd_output,
            evd
    );

    gtk_label_set_text(g_lbl_event_log, _("Processing..."));
    log_notice("running event '%s' on '%s'", event_name, g_dump_dir_name);
    char *msg = xasprintf("--- Running %s ---\n", event_name);
    append_to_textview(g_tv_event_log, msg);
    free(msg);

    /* don't bother testing if they are visible, this is faster */
    gtk_widget_hide(GTK_WIDGET(g_img_process_fail));

    gtk_widget_show(GTK_WIDGET(g_spinner_event_log));
    gtk_widget_show(g_btn_stop);
    /* Disable (gray out) navigation buttons */
    gtk_widget_set_sensitive(g_btn_close, false);
    gtk_widget_set_sensitive(g_btn_next, false);
}

/*
 * the widget is added as a child of the VBox in the warning area
 *
 */
static void add_widget_to_warning_area(GtkWidget *widget)
{
    g_warning_issued = true;
    gtk_box_pack_start(g_box_warning_labels, widget, false, false, 0);
    gtk_widget_show_all(widget);
}

/* Backtrace checkbox handling */

static void add_warning(const char *warning)
{
    char *label_str = xasprintf("• %s", warning);
    GtkWidget *warning_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(warning_lbl), label_str);
    /* should be safe to free it, gtk calls strdup() to copy it */
    free(label_str);

#if ((GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 13) || (GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION == 13 && GTK_MICRO_VERSION < 5))
    gtk_misc_set_alignment(GTK_MISC(warning_lbl), 0.0, 0.0);
#else
    gtk_widget_set_halign (warning_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign (warning_lbl, GTK_ALIGN_END);
#endif
    gtk_label_set_justify(GTK_LABEL(warning_lbl), GTK_JUSTIFY_LEFT);
    gtk_label_set_line_wrap(GTK_LABEL(warning_lbl), TRUE);

    add_widget_to_warning_area(warning_lbl);
}

static void on_sensitive_ticket_clicked_cb(GtkWidget *button, gpointer user_data)
{
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
    {
        xsetenv(CREATE_PRIVATE_TICKET, "1");
    }
    else
    {
        safe_unsetenv(CREATE_PRIVATE_TICKET);
    }
}

static void add_sensitive_data_warning(void)
{
    GtkBuilder *builder = make_builder();

    GtkWidget *sens_data_warn = GTK_WIDGET(gtk_builder_get_object(builder, SENSITIVE_DATA_WARN));
    GtkButton *sens_ticket_cb = GTK_BUTTON(gtk_builder_get_object(builder, PRIVATE_TICKET_CB));

    g_signal_connect(sens_ticket_cb, "toggled", G_CALLBACK(on_sensitive_ticket_clicked_cb), NULL);
    add_widget_to_warning_area(GTK_WIDGET(sens_data_warn));

    g_object_unref(builder);
}

static void show_warnings(void)
{
    if (g_warning_issued)
        gtk_widget_show(g_widget_warnings_area);
}

static void clear_warnings(void)
{
    /* erase all warnings */
    if (!g_warning_issued)
        return;

    gtk_widget_hide(g_widget_warnings_area);
    gtk_container_foreach(GTK_CONTAINER(g_box_warning_labels), &remove_child_widget, NULL);
    g_warning_issued = false;
}

/* TODO : this function should not set a warning directly, it makes the function unusable for add_event_buttons(); */
static bool check_minimal_bt_rating(const char *event_name)
{
    bool acceptable_rating = true;
    event_config_t *event_cfg = NULL;

    if (!event_name)
        error_msg_and_die(_("Cannot check backtrace rating because of invalid event name"));
    else if (prefixcmp(event_name, "report") != 0)
    {
        log_info("No checks for bactrace rating because event '%s' doesn't report.", event_name);
        return acceptable_rating;
    }
    else
        event_cfg = get_event_config(event_name);

    char *description = NULL;
    acceptable_rating = check_problem_rating_usability(event_cfg, g_cd, &description, NULL);
    if (description)
    {
        add_warning(description);
        free(description);
    }

    return acceptable_rating;
}

static void on_bt_approve_toggle(GtkToggleButton *togglebutton, gpointer user_data)
{
    gtk_widget_set_sensitive(g_btn_next, gtk_toggle_button_get_active(g_tb_approve_bt));
}

static void toggle_eb_comment(void)
{
    /* The page doesn't exist with report-only option */
    if (pages[PAGENO_EDIT_COMMENT].page_widget == NULL)
        return;

    bool good =
        gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(g_tv_comment)) >= 10
        || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_cb_no_comment));

    /* Allow next page only when the comment has at least 10 chars */
    gtk_widget_set_sensitive(g_btn_next, good);

    /* And show the eventbox with label */
    if (good)
        gtk_widget_hide(GTK_WIDGET(g_eb_comment));
    else
        gtk_widget_show(GTK_WIDGET(g_eb_comment));
}

static void on_comment_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    toggle_eb_comment();
}

static void on_no_comment_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
    toggle_eb_comment();
}

static void on_log_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    gtk_widget_show(GTK_WIDGET(g_exp_report_log));
}

#if 0
static void log_ready_state(void)
{
    char buf[NUM_PAGES+1];
    for (int i = 0; i < NUM_PAGES; i++)
    {
        char ch = '_';
        if (pages[i].page_widget)
            ch = gtk_assistant_get_page_complete(g_assistant, pages[i].page_widget) ? '+' : '-';
        buf[i] = ch;
    }
    buf[NUM_PAGES] = 0;
    log("Completeness:[%s]", buf);
}
#endif

static GList *find_words_in_text_buffer(int page,
                                        GtkTextView *tev,
                                        GList *words,
                                        GList *ignore_sitem_list,
                                        GtkTextIter start_find,
                                        GtkTextIter end_find,
                                        bool case_insensitive
                                        )
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(tev);
    gtk_text_buffer_set_modified(buffer, FALSE);

    GList *found_words = NULL;
    GtkTextIter start_match;
    GtkTextIter end_match;

    for (GList *w = words; w; w = g_list_next(w))
    {
        gtk_text_buffer_get_start_iter(buffer, &start_find);

        const char *search_word = w->data;
        while (search_word && search_word[0] && gtk_text_iter_forward_search(&start_find, search_word,
                    GTK_TEXT_SEARCH_TEXT_ONLY | (case_insensitive ? GTK_TEXT_SEARCH_CASE_INSENSITIVE : 0),
                    &start_match,
                    &end_match, NULL))
        {
            search_item_t *found_word = sitem_new(
                    page,
                    buffer,
                    tev,
                    start_match,
                    end_match
                );
            int offset = gtk_text_iter_get_offset(&end_match);
            gtk_text_buffer_get_iter_at_offset(buffer, &start_find, offset);

            if (sitem_is_in_sitemlist(found_word, ignore_sitem_list))
            {
                sitem_free(found_word);
                // don't count the word if it's part of some of the ignored words
                continue;
            }

            found_words = g_list_prepend(found_words, found_word);
        }
    }

    return found_words;
}

static void search_item_to_list_store_item(GtkListStore *store, GtkTreeIter *new_row,
        const gchar *file_name, search_item_t *word)
{
    GtkTextIter *beg = gtk_text_iter_copy(&(word->start));
    gtk_text_iter_backward_line(beg);

    GtkTextIter *end = gtk_text_iter_copy(&(word->end));
    /* the first call moves end variable at the end of the current line */
    if (gtk_text_iter_forward_line(end))
    {
        /* the second call moves end variable at the end of the next line */
        gtk_text_iter_forward_line(end);

        /* don't include the last new which causes an empty line in the GUI list */
        gtk_text_iter_backward_char(end);
    }

    gchar *tmp = gtk_text_buffer_get_text(word->buffer, beg, &(word->start),
            /*don't include hidden chars*/FALSE);
    gchar *prefix = g_markup_escape_text(tmp, /*NULL terminated string*/-1);
    g_free(tmp);

    tmp = gtk_text_buffer_get_text(word->buffer, &(word->start), &(word->end),
            /*don't include hidden chars*/FALSE);
    gchar *text = g_markup_escape_text(tmp, /*NULL terminated string*/-1);
    g_free(tmp);

    tmp = gtk_text_buffer_get_text(word->buffer, &(word->end), end,
            /*don't include hidden chars*/FALSE);
    gchar *suffix = g_markup_escape_text(tmp, /*NULL terminated string*/-1);
    g_free(tmp);

    char *content = xasprintf("%s<span foreground=\"red\">%s</span>%s", prefix, text, suffix);

    g_free(suffix);
    g_free(text);
    g_free(prefix);

    gtk_text_iter_free(end);
    gtk_text_iter_free(beg);

    gtk_list_store_set(store, new_row,
            SEARCH_COLUMN_FILE, file_name,
            SEARCH_COLUMN_TEXT, content,
            SEARCH_COLUMN_ITEM, word,
            -1);
}

static bool highligh_words_in_textview(int page, GtkTextView *tev, GList *words, GList *ignored_words, bool case_insensitive)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(tev);
    gtk_text_buffer_set_modified(buffer, FALSE);

    GtkWidget *notebook_child = gtk_notebook_get_nth_page(g_notebook, page);
    GtkWidget *tab_lbl = gtk_notebook_get_tab_label(g_notebook, notebook_child);

    /* Remove old results */
    bool buffer_removing = false;

    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_ls_sensitive_list), &iter);

    /* Turn off the changed callback during the update */
    g_signal_handler_block(g_tv_sensitive_sel, g_tv_sensitive_sel_hndlr);

    while (valid)
    {
        char *text = NULL;
        search_item_t *word = NULL;

        gtk_tree_model_get(GTK_TREE_MODEL(g_ls_sensitive_list), &iter,
                SEARCH_COLUMN_TEXT, &text,
                SEARCH_COLUMN_ITEM, &word,
                -1);

        free(text);

        if (word->buffer == buffer)
        {
            buffer_removing = true;

            valid = gtk_list_store_remove(g_ls_sensitive_list, &iter);

            if (word == g_current_highlighted_word)
                g_current_highlighted_word = NULL;

            free(word);
        }
        else
        {
            if(buffer_removing)
                break;

            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_ls_sensitive_list), &iter);
        }
    }

    /* Turn on the changed callback after the update */
    g_signal_handler_unblock(g_tv_sensitive_sel, g_tv_sensitive_sel_hndlr);

    GtkTextIter start_find;
    gtk_text_buffer_get_start_iter(buffer, &start_find);
    GtkTextIter end_find;
    gtk_text_buffer_get_end_iter(buffer, &end_find);

    gtk_text_buffer_remove_tag_by_name(buffer, "search_result_bg", &start_find, &end_find);
    gtk_text_buffer_remove_tag_by_name(buffer, "current_result_bg", &start_find, &end_find);

    PangoAttrList *attrs = gtk_label_get_attributes(GTK_LABEL(tab_lbl));
    gtk_label_set_attributes(GTK_LABEL(tab_lbl), NULL);
    pango_attr_list_unref(attrs);

    GList *result = NULL;
    GList *ignored_words_in_buffer = NULL;

    ignored_words_in_buffer = find_words_in_text_buffer(page,
                                                        tev,
                                                        ignored_words,
                                                        NULL,
                                                        start_find,
                                                        end_find,
                                                        /*case sensitive*/false);


    result = find_words_in_text_buffer(page,
                                       tev,
                                       words,
                                       ignored_words_in_buffer,
                                       start_find,
                                       end_find,
                                       case_insensitive
                                        );

    for (GList *w = result; w; w = g_list_next(w))
    {
        search_item_t *item = (search_item_t *)w->data;
        gtk_text_buffer_apply_tag_by_name(buffer, "search_result_bg",
                                          sitem_get_start_iter(item),
                                          sitem_get_end_iter(item));
    }

    if (result)
    {
        PangoAttrList *attrs = pango_attr_list_new();
        PangoAttribute *foreground_attr = pango_attr_foreground_new(65535, 0, 0);
        pango_attr_list_insert(attrs, foreground_attr);
        PangoAttribute *underline_attr = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
        pango_attr_list_insert(attrs, underline_attr);
        gtk_label_set_attributes(GTK_LABEL(tab_lbl), attrs);

        /* The current order of the found words is defined by order of words in the
         * passed list. We have to order the words according to their occurrence in
         * the buffer.
         */
        result = g_list_sort(result, (GCompareFunc)sitem_compare);

        GList *search_result = result;
        for ( ; search_result != NULL; search_result = g_list_next(search_result))
        {
            search_item_t *word = (search_item_t *)search_result->data;

            const gchar *file_name = gtk_label_get_text(GTK_LABEL(tab_lbl));

            /* Create a new row */
            GtkTreeIter new_row;
            if (valid)
                /* iter variable is valid GtkTreeIter and it means that the results */
                /* need to be inserted before this iterator, in this case iter points */
                /* to the first word of another GtkTextView */
                gtk_list_store_insert_before(g_ls_sensitive_list, &new_row, &iter);
            else
                /* the GtkTextView is the last one or the only one, insert the results */
                /* at the end of the list store */
                gtk_list_store_append(g_ls_sensitive_list, &new_row);

            /* Assign values to the new row */
            search_item_to_list_store_item(g_ls_sensitive_list, &new_row, file_name, word);
        }
    }

    g_list_free_full(ignored_words_in_buffer, free);
    g_list_free(result);

    return result != NULL;
}

static gboolean highligh_words_in_tabs(GList *forbidden_words,  GList *allowed_words, bool case_insensitive)
{
    gboolean found = false;

    gint n_pages = gtk_notebook_get_n_pages(g_notebook);
    int page = 0;
    for (page = 0; page < n_pages; page++)
    {
        //notebook_page->scrolled_window->text_view
        GtkWidget *notebook_child = gtk_notebook_get_nth_page(g_notebook, page);
        GtkWidget *tab_lbl = gtk_notebook_get_tab_label(g_notebook, notebook_child);

        const char *const lbl_txt = gtk_label_get_text(GTK_LABEL(tab_lbl));
        if (strncmp(lbl_txt, "page 1", 5) == 0 || strcmp(FILENAME_COMMENT, lbl_txt) == 0)
            continue;

        GtkTextView *tev = GTK_TEXT_VIEW(gtk_bin_get_child(GTK_BIN(notebook_child)));
        found |= highligh_words_in_textview(page, tev, forbidden_words, allowed_words, case_insensitive);
    }

    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_ls_sensitive_list), &iter))
        gtk_tree_selection_select_iter(g_tv_sensitive_sel, &iter);

    return found;
}

static gboolean highlight_forbidden(void)
{
    GList *forbidden_words = load_words_from_file(FORBIDDEN_WORDS_BLACKLLIST);
    GList *allowed_words = load_words_from_file(FORBIDDEN_WORDS_WHITELIST);

    const gboolean result = highligh_words_in_tabs(forbidden_words, allowed_words, /*case sensitive*/false);

    list_free_with_free(forbidden_words);
    list_free_with_free(allowed_words);

    return result;
}

static char *get_next_processed_event(GList **events_list)
{
    if (!events_list || !*events_list)
        return NULL;

    char *event_name = (char *)(*events_list)->data;
    const size_t event_len = strlen(event_name);

    /* pop the current event */
    *events_list = g_list_delete_link(*events_list, *events_list);

    if (event_name[event_len - 1] == '*')
    {
        log_info("Expanding event '%s'", event_name);

        struct dump_dir *dd = dd_opendir(g_dump_dir_name, DD_OPEN_READONLY);
        if (!dd)
            error_msg_and_die("Can't open directory '%s'", g_dump_dir_name);

        /* Erase '*' */
        event_name[event_len - 1] = '\0';

        /* get 'event1\nevent2\nevent3\n' or '' if no event is possible */
        char *expanded_events = list_possible_events(dd, g_dump_dir_name, event_name);

        dd_close(dd);
        free(event_name);

        GList *expanded_list = NULL;
        /* add expanded events from event having trailing '*' */
        char *next = event_name = expanded_events;
        while ((next = strchr(event_name, '\n')))
        {
            /* 'event1\0event2\nevent3\n' */
            next[0] = '\0';

            /* 'event1' */
            event_name = xstrdup(event_name);
            log_debug("Adding a new expanded event '%s' to the processed list", event_name);

            /* the last event is not added to the expanded list */
            ++next;
            if (next[0] == '\0')
                break;

            expanded_list = g_list_prepend(expanded_list, event_name);

            /* 'event2\nevent3\n' */
            event_name = next;
        }

        free(expanded_events);

        /* It's OK we can safely compare address even if them were previously freed */
        if (event_name != expanded_events)
            /* the last expanded event name is stored in event_name */
            *events_list = g_list_concat(expanded_list, *events_list);
        else
        {
            log_info("No event was expanded, will continue with the next one.");
            /* no expanded event try the next event */
            return get_next_processed_event(events_list);
        }
    }

    clear_warnings();
    const bool acceptable = check_minimal_bt_rating(event_name);
    show_warnings();

    if (!acceptable)
    {
        /* a node for this event was already removed */
        free(event_name);

        g_list_free_full(*events_list, free);
        *events_list = NULL;
        return NULL;
    }

    return event_name;
}

static void on_page_prepare(GtkNotebook *assistant, GtkWidget *page, gpointer user_data)
{
    //int page_no = gtk_assistant_get_current_page(g_assistant);
    //log_ready_state();

    /* This suppresses [Last] button: assistant thinks that
     * we never have this page ready unless we are on it
     * -> therefore there is at least one non-ready page
     * -> therefore it won't show [Last]
     */
    // Doesn't work: if Completeness:[++++++-+++],
    // then [Last] btn will still be shown.
    //gtk_assistant_set_page_complete(g_assistant,
    //            pages[PAGENO_REVIEW_DATA].page_widget,
    //            pages[PAGENO_REVIEW_DATA].page_widget == page
    //);

    /* If processing is finished and if it was terminated because of an error
     * the event progress page is selected. So, it does not make sense to show
     * the next step button and we MUST NOT clear warnings.
     */
    if (!is_processing_finished())
    {
        /* some pages hide it, so restore it to it's default */
        show_next_step_button();
        clear_warnings();
    }

    gtk_widget_hide(g_btn_detail);
    gtk_widget_hide(g_btn_onfail);
    if (!g_expert_mode)
        gtk_widget_hide(g_btn_repeat);
    /* Save text fields if changed */
    /* Must be called before any GUI operation because the following two
     * functions causes recreating of the text items tabs, thus all updates to
     * these tabs will be lost */
    save_items_from_notepad();
    save_text_from_text_view(g_tv_comment, FILENAME_COMMENT);
    problem_data_reload_from_dump_dir();
    update_gui_state_from_problem_data(/* don't update selected event */ 0);

    if (pages[PAGENO_SUMMARY].page_widget == page)
    {
        if (!g_expert_mode)
        {
            /* Skip intro screen */
            int n = select_next_page_no(pages[PAGENO_SUMMARY].page_no, NULL);
            log_info("switching to page_no:%d", n);
            gtk_notebook_set_current_page(assistant, n);
            return;
        }
    }

    if (pages[PAGENO_EDIT_ELEMENTS].page_widget == page)
    {
        if (highlight_forbidden())
        {
            add_sensitive_data_warning();
            show_warnings();
            gtk_expander_set_expanded(g_exp_search, TRUE);
        }
        else
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_rb_custom_search), TRUE);

        show_warnings();
    }

    if (pages[PAGENO_REVIEW_DATA].page_widget == page)
    {
        update_ls_details_checkboxes(g_event_selected);
        gtk_widget_set_sensitive(g_btn_next, gtk_toggle_button_get_active(g_tb_approve_bt));
    }

    if (pages[PAGENO_EDIT_COMMENT].page_widget == page)
    {
        gtk_widget_show(g_btn_detail);
        gtk_widget_set_sensitive(g_btn_next, false);
        on_comment_changed(gtk_text_view_get_buffer(g_tv_comment), NULL);
    }
    //log_ready_state();

    if (pages[PAGENO_EVENT_PROGRESS].page_widget == page)
    {
        log_info("g_event_selected:'%s'", g_event_selected);
        if (g_event_selected
         && g_event_selected[0]
        ) {
            clear_warnings();
            start_event_run(g_event_selected);
        }
    }

    if(pages[PAGENO_EVENT_SELECTOR].page_widget == page)
    {
        if (!g_expert_mode && !g_auto_event_list)
            hide_next_step_button();
    }
}

//static void event_rb_was_toggled(GtkButton *button, gpointer user_data)
static void set_auto_event_chain(GtkButton *button, gpointer user_data)
{
    //event is selected, so make sure the expert mode is disabled
    g_expert_mode = false;

    workflow_t *w = (workflow_t *)user_data;
    config_item_info_t *info = workflow_get_config_info(w);
    log_notice("selected workflow '%s'", ci_get_screen_name(info));

    GList *wf_event_list = wf_get_event_list(w);
    while(wf_event_list)
    {
        g_auto_event_list = g_list_append(g_auto_event_list, xstrdup(ec_get_name(wf_event_list->data)));
        load_single_event_config_data_from_user_storage((event_config_t *)wf_event_list->data);

        wf_event_list = g_list_next(wf_event_list);
    }

    gint current_page_no = gtk_notebook_get_current_page(g_assistant);
    gint next_page_no = select_next_page_no(current_page_no, NULL);

    /* if pageno is not change 'switch-page' signal is not emitted */
    if (current_page_no == next_page_no)
        on_page_prepare(g_assistant, gtk_notebook_get_nth_page(g_assistant, next_page_no), NULL);
    else
        gtk_notebook_set_current_page(g_assistant, next_page_no);

    /* Show Next Step button which was hidden on Selector page in non-expert
     * mode. Next Step button must be hidden because Selector page shows only
     * workflow buttons in non-expert mode.
     */
    show_next_step_button();
}

static void add_workflow_buttons(GtkBox *box, GHashTable *workflows, GCallback func)
{
    gtk_container_foreach(GTK_CONTAINER(box), &remove_child_widget, NULL);

    GList *possible_workflows = list_possible_events_glist(g_dump_dir_name, "workflow");
    GHashTable *workflow_table = load_workflow_config_data_from_list(
                        possible_workflows,
                        WORKFLOWS_DIR);
    g_list_free_full(possible_workflows, free);
    g_object_set_data_full(G_OBJECT(box), "workflows", workflow_table, (GDestroyNotify)g_hash_table_destroy);

    GList *wf_list = g_hash_table_get_values(workflow_table);
    wf_list = g_list_sort(wf_list, (GCompareFunc)wf_priority_compare);

    for (GList *wf_iter = wf_list; wf_iter; wf_iter = g_list_next(wf_iter))
    {
        workflow_t *w = (workflow_t *)wf_iter->data;
        char *btn_label = xasprintf("<b>%s</b>\n%s", wf_get_screen_name(w), wf_get_description(w));
        GtkWidget *button = gtk_button_new_with_label(btn_label);
        GList *children = gtk_container_get_children(GTK_CONTAINER(button));
        GtkWidget *label = (GtkWidget *)children->data;
        gtk_label_set_use_markup(GTK_LABEL(label), true);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_margin_top(label, 10);
#if ((GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 11) || (GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION == 11 && GTK_MICRO_VERSION < 2))
        gtk_widget_set_margin_left(label, 40);
#else
        gtk_widget_set_margin_start(label, 40);
#endif
        gtk_widget_set_margin_bottom(label, 10);
        g_list_free(children);
        free(btn_label);
        g_signal_connect(button, "clicked", func, w);
        gtk_box_pack_start(box, button, true, false, 2);
    }

    g_list_free(wf_list);
}

static char *setup_next_processed_event(GList **events_list)
{
    free(g_event_selected);
    g_event_selected = NULL;

    char *event = get_next_processed_event(&g_auto_event_list);
    if (!event)
    {
        /* No next event, go to progress page and finish */
        gtk_label_set_text(g_lbl_event_log, _("Processing finished."));
        /* we don't know the result of the previous event here
         * so at least hide the spinner, because we're obviously finished
        */
        gtk_widget_hide(GTK_WIDGET(g_spinner_event_log));
        hide_next_step_button();
        return NULL;
    }

    log_notice("selected -e EVENT:%s", event);
    return event;
}

static bool get_sensitive_data_permission(const char *event_name)
{
    event_config_t *event_cfg = get_event_config(event_name);

    if (!event_cfg || !event_cfg->ec_sending_sensitive_data)
        return true;

    char *msg = xasprintf(_("Event '%s' requires permission to send possibly sensitive data."
                            "\nDo you want to continue?"),
                            ec_get_screen_name(event_cfg) ? ec_get_screen_name(event_cfg) : event_name);
    const bool response = run_ask_yes_no_yesforever_dialog("ask_send_sensitive_data", msg, GTK_WINDOW(g_wnd_assistant));
    free(msg);

    return response;
}

static gint select_next_page_no(gint current_page_no, gpointer data)
{
    GtkWidget *page;

 again:
    log_notice("%s: current_page_no:%d", __func__, current_page_no);
    current_page_no++;
    page = gtk_notebook_get_nth_page(g_assistant, current_page_no);

    if (pages[PAGENO_EVENT_SELECTOR].page_widget == page)
    {
        if (!g_expert_mode && (g_auto_event_list == NULL))
        {
            return current_page_no; //stay here and let user select the workflow
        }

        if (!g_expert_mode)
        {
            /* (note: this frees and sets to NULL g_event_selected) */
            char *event = setup_next_processed_event(&g_auto_event_list);
            if (!event)
            {
                current_page_no = pages[PAGENO_EVENT_PROGRESS].page_no - 1;
                goto again;
            }

            if (!get_sensitive_data_permission(event))
            {
                free(event);

                cancel_processing(g_lbl_event_log, /* default message */ NULL, TERMINATE_NOFLAGS);
                current_page_no = pages[PAGENO_EVENT_PROGRESS].page_no - 1;
                goto again;
            }

            if (problem_data_get_content_or_NULL(g_cd, FILENAME_NOT_REPORTABLE))
            {
                free(event);

                char *msg = xasprintf(_("This problem should not be reported "
                                "(it is likely a known problem). %s"),
                                problem_data_get_content_or_NULL(g_cd, FILENAME_NOT_REPORTABLE)
                );
                cancel_processing(g_lbl_event_log, msg, TERMINATE_NOFLAGS);
                free(msg);
                current_page_no = pages[PAGENO_EVENT_PROGRESS].page_no - 1;
                goto again;
            }

            g_event_selected = event;

            /* Notify a user that some configuration options miss values, but */
            /* don't force him to provide them. */
            check_event_config(g_event_selected);

            /* >>> and this but this is clearer
             * because it does exactly the same thing
             * but I'm pretty scared to touch it */
            current_page_no = pages[PAGENO_EVENT_SELECTOR].page_no + 1;
            goto event_was_selected;
        }
    }

    if (pages[PAGENO_EVENT_SELECTOR + 1].page_widget == page)
    {
 event_was_selected:
        if (!g_event_selected)
        {
            /* Go back to selectors */
            current_page_no = pages[PAGENO_EVENT_SELECTOR].page_no - 1;
            goto again;
        }

        if (!event_need_review(g_event_selected))
        {
            current_page_no = pages[PAGENO_EVENT_PROGRESS].page_no - 1;
            goto again;
        }
    }

#if 0
    if (pages[PAGENO_EDIT_COMMENT].page_widget == page)
    {
        if (problem_data_get_content_or_NULL(g_cd, FILENAME_COMMENT))
            goto again; /* no comment, skip this page */
    }
#endif

    if (pages[PAGENO_EVENT_DONE].page_widget == page)
    {
        if (g_auto_event_list)
        {
            /* Go back to selectors */
            current_page_no = pages[PAGENO_SUMMARY].page_no;
        }
        goto again;
    }

    if (pages[PAGENO_NOT_SHOWN].page_widget == page)
    {
        if (!g_expert_mode)
            exit(0);
        /* No! this would SEGV (infinitely recurse into select_next_page_no) */
        /*gtk_assistant_commit(g_assistant);*/
        current_page_no = pages[PAGENO_EVENT_SELECTOR].page_no - 1;
        goto again;
    }

    log_notice("%s: selected page #%d", __func__, current_page_no);
    return current_page_no;
}

static void rehighlight_forbidden_words(int page, GtkTextView *tev)
{
    GList *forbidden_words = load_words_from_file(FORBIDDEN_WORDS_BLACKLLIST);
    GList *allowed_words = load_words_from_file(FORBIDDEN_WORDS_WHITELIST);
    highligh_words_in_textview(page, tev, forbidden_words, allowed_words, /*case sensitive*/false);
    list_free_with_free(forbidden_words);
    list_free_with_free(allowed_words);
}

static void on_sensitive_word_selection_changed(GtkTreeSelection *sel, gpointer user_data)
{
    search_item_t *old_word = g_current_highlighted_word;
    g_current_highlighted_word = NULL;

    if (old_word && FALSE == gtk_text_buffer_get_modified(old_word->buffer))
        gtk_text_buffer_remove_tag_by_name(old_word->buffer, "current_result_bg", &(old_word->start), &(old_word->end));

    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;

    search_item_t *new_word;
    gtk_tree_model_get(model, &iter,
            SEARCH_COLUMN_ITEM, &new_word,
            -1);

    if (gtk_text_buffer_get_modified(new_word->buffer))
    {
        if (g_search_text == NULL)
            rehighlight_forbidden_words(new_word->page, new_word->tev);
        else
        {
            log_notice("searching again: '%s'", g_search_text);
            GList *searched_words = g_list_append(NULL, (gpointer)g_search_text);
            highligh_words_in_textview(new_word->page, new_word->tev, searched_words, NULL, /*case insensitive*/true);
            g_list_free(searched_words);
        }

        return;
    }

    g_current_highlighted_word = new_word;

    gtk_notebook_set_current_page(g_notebook, new_word->page);
    gtk_text_buffer_apply_tag_by_name(new_word->buffer, "current_result_bg", &(new_word->start), &(new_word->end));
    gtk_text_buffer_place_cursor(new_word->buffer, &(new_word->start));
    gtk_text_view_scroll_to_iter(new_word->tev, &(new_word->start), 0.0, false, 0, 0);
}

static void highlight_search(GtkEntry *entry)
{
    g_search_text = gtk_entry_get_text(entry);

    log_notice("searching: '%s'", g_search_text);
    GList *words = g_list_append(NULL, (gpointer)g_search_text);
    highligh_words_in_tabs(words, NULL, /*case insensitive*/true);
    g_list_free(words);
}

static gboolean highlight_search_on_timeout(gpointer user_data)
{
    g_timeout = 0;
    highlight_search(GTK_ENTRY(user_data));
    /* returning false will make glib to remove this event */
    return false;
}

static void search_timeout(GtkEntry *entry)
{
    /* this little hack makes the search start searching after 500 milisec after
     * user stops writing into entry box
     * if this part is removed, then the search will be started on every
     * change of the search entry
     */
    if (g_timeout != 0)
        g_source_remove(g_timeout);
    g_timeout = g_timeout_add(500, &highlight_search_on_timeout, (gpointer)entry);
}

static void on_forbidden_words_toggled(GtkToggleButton *btn, gpointer user_data)
{
    g_search_text = NULL;
    log_notice("nothing to search for, highlighting forbidden words instead");
    highlight_forbidden();
}

static void on_custom_search_toggled(GtkToggleButton *btn, gpointer user_data)
{
    const gboolean custom_search = gtk_toggle_button_get_active(btn);
    gtk_widget_set_sensitive(GTK_WIDGET(g_search_entry_bt), custom_search);

    if (custom_search)
        highlight_search(g_search_entry_bt);
}

static void save_edited_one_liner(GtkCellRendererText *renderer,
                gchar *tree_path,
                gchar *new_text,
                gpointer user_data)
{
    //log("path:'%s' new_text:'%s'", tree_path, new_text);

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(g_ls_details), &iter, tree_path))
        return;
    gchar *item_name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(g_ls_details), &iter,
                DETAIL_COLUMN_NAME, &item_name,
                -1);
    if (!item_name) /* paranoia, should never happen */
        return;
    struct problem_item *item = problem_data_get_item_or_NULL(g_cd, item_name);
    if (item && (item->flags & CD_FLAG_ISEDITABLE))
    {
        struct dump_dir *dd = wizard_open_directory_for_writing(g_dump_dir_name);
        if (dd)
        {
            dd_save_text(dd, item_name, new_text);
            free(item->content);
            item->content = xstrdup(new_text);
            gtk_list_store_set(g_ls_details, &iter,
                    DETAIL_COLUMN_VALUE, new_text,
                    -1);
        }
        dd_close(dd);
    }
}

static void on_btn_add_file(GtkButton *button)
{
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
            "Attach File",
            GTK_WINDOW(g_wnd_assistant),
            GTK_FILE_CHOOSER_ACTION_OPEN,
            _("_Cancel"), GTK_RESPONSE_CANCEL,
            _("_Open"), GTK_RESPONSE_ACCEPT,
            NULL
    );
    char *filename = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    gtk_widget_destroy(dialog);

    if (filename)
    {
        char *basename = strrchr(filename, '/');
        if (!basename)  /* wtf? (never happens) */
            goto free_and_ret;
        basename++;

        /* TODO: ask for the name to save it as? For now, just use basename */

        char *message = NULL;

        struct stat statbuf;
        if (stat(filename, &statbuf) != 0 || !S_ISREG(statbuf.st_mode))
        {
            message = xasprintf(_("'%s' is not an ordinary file"), filename);
            goto show_msgbox;
        }

        struct problem_item *item = problem_data_get_item_or_NULL(g_cd, basename);
        if (!item || (item->flags & CD_FLAG_ISEDITABLE))
        {
            struct dump_dir *dd = wizard_open_directory_for_writing(g_dump_dir_name);
            if (dd)
            {
                dd_close(dd);
                char *new_name = concat_path_file(g_dump_dir_name, basename);
                if (strcmp(filename, new_name) == 0)
                {
                    message = xstrdup(_("You are trying to copy a file onto itself"));
                }
                else
                {
                    off_t r = copy_file(filename, new_name, 0666);
                    if (r < 0)
                    {
                        message = xasprintf(_("Can't copy '%s': %s"), filename, strerror(errno));
                        unlink(new_name);
                    }
                    if (!message)
                    {
                        problem_data_reload_from_dump_dir();
                        update_gui_state_from_problem_data(/* don't update selected event */ 0);
                        /* Set flags for the new item */
                        update_ls_details_checkboxes(g_event_selected);
                    }
                }
                free(new_name);
            }
        }
        else
            message = xasprintf(_("Item '%s' already exists and is not modifiable"), basename);

        if (message)
        {
 show_msgbox: ;
            GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_wnd_assistant),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_WARNING,
                GTK_BUTTONS_CLOSE,
                message);
            free(message);
            gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(g_wnd_assistant));
            gtk_dialog_run(GTK_DIALOG(dlg));
            gtk_widget_destroy(dlg);
        }
 free_and_ret:
        g_free(filename);
    }
}

static void on_btn_detail(GtkButton *button)
{
    GtkWidget *pdd = problem_details_dialog_new(g_cd, g_wnd_assistant);
    gtk_dialog_run(GTK_DIALOG(pdd));
}

/* [Del] key handling in item list */
static void delete_item(GtkTreeView *treeview)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(treeview);
    if (selection)
    {
        GtkTreeIter iter;
        GtkTreeModel *store = gtk_tree_view_get_model(treeview);
        if (gtk_tree_selection_get_selected(selection, &store, &iter) == TRUE)
        {
            GValue d_item_name = { 0 };
            gtk_tree_model_get_value(store, &iter, DETAIL_COLUMN_NAME, &d_item_name);
            const char *item_name = g_value_get_string(&d_item_name);
            if (item_name)
            {
                struct problem_item *item = problem_data_get_item_or_NULL(g_cd, item_name);
                if (item->flags & CD_FLAG_ISEDITABLE)
                {
//                  GtkTreePath *old_path = gtk_tree_model_get_path(store, &iter);

                    struct dump_dir *dd = wizard_open_directory_for_writing(g_dump_dir_name);
                    if (dd)
                    {
                        char *filename = concat_path_file(g_dump_dir_name, item_name);
                        unlink(filename);
                        free(filename);
                        dd_close(dd);
                        g_hash_table_remove(g_cd, item_name);
                        gtk_list_store_remove(g_ls_details, &iter);
                    }

//                  /* Try to retain the same cursor position */
//                  sanitize_cursor(old_path);
//                  gtk_tree_path_free(old_path);
                }
            }
        }
    }
}
static gint on_key_press_event_in_item_list(GtkTreeView *treeview, GdkEventKey *key, gpointer unused)
{
    int k = key->keyval;

    if (k == GDK_KEY_Delete || k == GDK_KEY_KP_Delete)
    {
        delete_item(treeview);
        return TRUE;
    }
    return FALSE;
}

/* Initialization */

static void on_next_btn_cb(GtkWidget *btn, gpointer user_data)
{
    gint current_page_no = gtk_notebook_get_current_page(g_assistant);
    gint next_page_no = select_next_page_no(current_page_no, NULL);

    /* if pageno is not change 'switch-page' signal is not emitted */
    if (current_page_no == next_page_no)
        on_page_prepare(g_assistant, gtk_notebook_get_nth_page(g_assistant, next_page_no), NULL);
    else
        gtk_notebook_set_current_page(g_assistant, next_page_no);
}

static void add_pages(void)
{
    g_builder = make_builder();

    int i;
    int page_no = 0;
    for (i = 0; page_names[i] != NULL; i++)
    {
        GtkWidget *page = GTK_WIDGET(gtk_builder_get_object(g_builder, page_names[i]));
        pages[i].page_widget = page;
        pages[i].page_no = page_no++;
        gtk_notebook_append_page(g_assistant, page, gtk_label_new(pages[i].title));
        log_notice("added page: %s", page_names[i]);
    }

    /* Set pointers to objects we might need to work with */
    g_lbl_cd_reason        = GTK_LABEL(        gtk_builder_get_object(g_builder, "lbl_cd_reason"));
    g_box_events           = GTK_BOX(          gtk_builder_get_object(g_builder, "vb_events"));
    g_box_workflows        = GTK_BOX(          gtk_builder_get_object(g_builder, "vb_workflows"));
    g_lbl_event_log        = GTK_LABEL(        gtk_builder_get_object(g_builder, "lbl_event_log"));
    g_tv_event_log         = GTK_TEXT_VIEW(    gtk_builder_get_object(g_builder, "tv_event_log"));
    g_tv_comment           = GTK_TEXT_VIEW(    gtk_builder_get_object(g_builder, "tv_comment"));
    g_eb_comment           = GTK_EVENT_BOX(    gtk_builder_get_object(g_builder, "eb_comment"));
    g_cb_no_comment        = GTK_CHECK_BUTTON( gtk_builder_get_object(g_builder, "cb_no_comment"));
    g_tv_details           = GTK_TREE_VIEW(    gtk_builder_get_object(g_builder, "tv_details"));
    g_tb_approve_bt        = GTK_TOGGLE_BUTTON(gtk_builder_get_object(g_builder, "cb_approve_bt"));
    g_search_entry_bt      = GTK_ENTRY(        gtk_builder_get_object(g_builder, "entry_search_bt"));
    g_container_details1   = GTK_CONTAINER(    gtk_builder_get_object(g_builder, "container_details1"));
    g_container_details2   = GTK_CONTAINER(    gtk_builder_get_object(g_builder, "container_details2"));
    g_btn_add_file         = GTK_BUTTON(       gtk_builder_get_object(g_builder, "btn_add_file"));
    g_lbl_size             = GTK_LABEL(        gtk_builder_get_object(g_builder, "lbl_size"));
    g_notebook             = GTK_NOTEBOOK(     gtk_builder_get_object(g_builder, "notebook_edit"));
    g_ls_sensitive_list    = GTK_LIST_STORE(   gtk_builder_get_object(g_builder, "ls_sensitive_words"));
    g_tv_sensitive_list    = GTK_TREE_VIEW(    gtk_builder_get_object(g_builder, "tv_sensitive_words"));
    g_tv_sensitive_sel     = GTK_TREE_SELECTION( gtk_builder_get_object(g_builder, "tv_sensitive_words_selection"));
    g_rb_forbidden_words   = GTK_RADIO_BUTTON( gtk_builder_get_object(g_builder, "rb_forbidden_words"));
    g_rb_custom_search     = GTK_RADIO_BUTTON( gtk_builder_get_object(g_builder, "rb_custom_search"));
    g_exp_search           = GTK_EXPANDER(     gtk_builder_get_object(g_builder, "expander_search"));
    g_spinner_event_log    = GTK_SPINNER(      gtk_builder_get_object(g_builder, "spinner_event_log"));
    g_img_process_fail     = GTK_IMAGE(      gtk_builder_get_object(g_builder, "img_process_fail"));
    g_btn_startcast        = GTK_BUTTON(    gtk_builder_get_object(g_builder, "btn_startcast"));
    g_exp_report_log       = GTK_EXPANDER(     gtk_builder_get_object(g_builder, "expand_report"));

    gtk_widget_set_no_show_all(GTK_WIDGET(g_spinner_event_log), true);

    gtk_widget_override_font(GTK_WIDGET(g_tv_event_log), g_monospace_font);
    fix_all_wrapped_labels(GTK_WIDGET(g_assistant));

    g_signal_connect(g_cb_no_comment, "toggled", G_CALLBACK(on_no_comment_toggled), NULL);

    g_signal_connect(g_rb_forbidden_words, "toggled", G_CALLBACK(on_forbidden_words_toggled), NULL);
    g_signal_connect(g_rb_custom_search, "toggled", G_CALLBACK(on_custom_search_toggled), NULL);

    /* Set color of the comment evenbox */
    GdkRGBA color;
    gdk_rgba_parse(&color, "#CC3333");
    gtk_widget_override_color(GTK_WIDGET(g_eb_comment), GTK_STATE_FLAG_NORMAL, &color);

    g_signal_connect(g_tv_details, "key-press-event", G_CALLBACK(on_key_press_event_in_item_list), NULL);
    g_tv_sensitive_sel_hndlr = g_signal_connect(g_tv_sensitive_sel, "changed", G_CALLBACK(on_sensitive_word_selection_changed), NULL);


}

static void create_details_treeview(void)
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;

    renderer = gtk_cell_renderer_toggle_new();
    column = gtk_tree_view_column_new_with_attributes(
                _("Include"), renderer,
                /* which "attr" of renderer to set from which COLUMN? (can be repeated) */
                "active", DETAIL_COLUMN_CHECKBOX,
                NULL);
    g_tv_details_col_checkbox = column;
    gtk_tree_view_append_column(g_tv_details, column);
    /* This column has a handler */
    g_signal_connect(renderer, "toggled", G_CALLBACK(g_tv_details_checkbox_toggled), NULL);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(
                _("Name"), renderer,
                "text", DETAIL_COLUMN_NAME,
                NULL);
    gtk_tree_view_append_column(g_tv_details, column);
    /* This column has a clickable header for sorting */
    gtk_tree_view_column_set_sort_column_id(column, DETAIL_COLUMN_NAME);

    g_tv_details_renderer_value = renderer = gtk_cell_renderer_text_new();
    g_signal_connect(renderer, "edited", G_CALLBACK(save_edited_one_liner), NULL);
    column = gtk_tree_view_column_new_with_attributes(
                _("Value"), renderer,
                "text", DETAIL_COLUMN_VALUE,
                NULL);
    gtk_tree_view_append_column(g_tv_details, column);
    /* This column has a clickable header for sorting */
    gtk_tree_view_column_set_sort_column_id(column, DETAIL_COLUMN_VALUE);

    /*
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(
                _("Path"), renderer,
                "text", DETAIL_COLUMN_PATH,
                NULL);
    gtk_tree_view_append_column(g_tv_details, column);
    */

    g_ls_details = gtk_list_store_new(DETAIL_NUM_COLUMNS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_view_set_model(g_tv_details, GTK_TREE_MODEL(g_ls_details));

    g_signal_connect(g_tv_details, "row-activated", G_CALLBACK(tv_details_row_activated), NULL);
    g_signal_connect(g_tv_details, "cursor-changed", G_CALLBACK(tv_details_cursor_changed), NULL);
    /* [Enter] on a row:
     * g_signal_connect(g_tv_details, "select-cursor-row", G_CALLBACK(tv_details_select_cursor_row), NULL);
     */
}

static void init_page(page_obj_t *page, const char *name, const char *title)
{
    page->name = name;
    page->title = title;
}

static void init_pages(void)
{
    init_page(&pages[0], PAGE_SUMMARY            , _("Problem description")   );
    init_page(&pages[1], PAGE_EVENT_SELECTOR     , _("Select how to report this problem"));
    init_page(&pages[2], PAGE_EDIT_COMMENT,_("Provide additional information"));
    init_page(&pages[3], PAGE_EDIT_ELEMENTS      , _("Review the data")       );
    init_page(&pages[4], PAGE_REVIEW_DATA        , _("Confirm data to report"));
    init_page(&pages[5], PAGE_EVENT_PROGRESS     , _("Processing")            );
    init_page(&pages[6], PAGE_EVENT_DONE         , _("Processing done")       );
//do we still need this?
    init_page(&pages[7], PAGE_NOT_SHOWN          , ""                         );
}

static void assistant_quit_cb(void *obj, void *data)
{
    /* Suppress execution of consume_cmd_output() */
    if (g_event_source_id != 0)
    {
        g_source_remove(g_event_source_id);
        g_event_source_id = 0;
    }

    cancel_event_run();

    if (g_loaded_texts)
    {
        g_hash_table_destroy(g_loaded_texts);
        g_loaded_texts = NULL;
    }

    gtk_widget_destroy(GTK_WIDGET(g_wnd_assistant));
    g_wnd_assistant = (void *)0xdeadbeaf;
}

static void on_btn_startcast(GtkWidget *btn, gpointer user_data)
{
    const char *args[15];
    args[0] = (char *) "fros";
    args[1] = NULL;

    pid_t castapp = 0;
    castapp = fork_execv_on_steroids(
                EXECFLG_QUIET,
                (char **)args,
                NULL,
                /*env_vec:*/ NULL,
                g_dump_dir_name,
                /*uid (ignored):*/ 0
    );
    gtk_widget_hide(GTK_WIDGET(g_wnd_assistant));
    /*flush all gui events before we start waitpid
     * otherwise the main window wouldn't hide
     */
    while (gtk_events_pending())
        gtk_main_iteration_do(false);

    int status;
    safe_waitpid(castapp, &status, 0);
    problem_data_reload_from_dump_dir();
    update_gui_state_from_problem_data(0 /* don't update the selected event */);
    gtk_widget_show(GTK_WIDGET(g_wnd_assistant));
}

static bool is_screencast_available()
{
    const char *args[3];
    args[0] = (char *) "fros";
    args[1] = "--is-available";
    args[2] = NULL;

    pid_t castapp = 0;
    castapp = fork_execv_on_steroids(
                EXECFLG_QUIET,
                (char **)args,
                NULL,
                /*env_vec:*/ NULL,
                g_dump_dir_name,
                /*uid (ignored):*/ 0
    );

    int status;
    safe_waitpid(castapp, &status, 0);

    /* 0 means that it's available */
    return status == 0;
}

void create_assistant(GtkApplication *app, bool expert_mode)
{
    g_loaded_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_expert_mode = expert_mode;

    g_monospace_font = pango_font_description_from_string("monospace");

    g_assistant = GTK_NOTEBOOK(gtk_notebook_new());

    /* Show tabs only in verbose expert mode
     *
     * It is safe to let users randomly switch tabs only in expert mode because
     * in all other modes a data for the selected page may not be ready and it
     * will probably cause unexpected behaviour like crash.
     */
    gtk_notebook_set_show_tabs(g_assistant, (g_verbose != 0 && g_expert_mode));

    g_btn_close = gtk_button_new_with_mnemonic(_("_Close"));
    gtk_button_set_image(GTK_BUTTON(g_btn_close), gtk_image_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON));
    g_btn_stop = gtk_button_new_with_mnemonic(_("_Stop"));
    gtk_button_set_image(GTK_BUTTON(g_btn_stop), gtk_image_new_from_icon_name("process-stop-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_no_show_all(g_btn_stop, true); /* else gtk_widget_hide won't work */
    g_btn_onfail = gtk_button_new_with_label(_("Upload for analysis"));
    gtk_button_set_image(GTK_BUTTON(g_btn_onfail), gtk_image_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_no_show_all(g_btn_onfail, true); /* else gtk_widget_hide won't work */
    g_btn_repeat = gtk_button_new_with_label(_("Repeat"));
    gtk_widget_set_no_show_all(g_btn_repeat, true); /* else gtk_widget_hide won't work */
    g_btn_next = gtk_button_new_with_mnemonic(_("_Forward"));
    gtk_button_set_image(GTK_BUTTON(g_btn_next), gtk_image_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_no_show_all(g_btn_next, true); /* else gtk_widget_hide won't work */
    g_btn_detail = gtk_button_new_with_mnemonic(_("Details"));
    gtk_widget_set_no_show_all(g_btn_detail, true); /* else gtk_widget_hide won't work */

    g_box_buttons = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_box_pack_start(g_box_buttons, g_btn_close, false, false, 5);
    gtk_box_pack_start(g_box_buttons, g_btn_stop, false, false, 5);
    gtk_box_pack_start(g_box_buttons, g_btn_onfail, false, false, 5);
    gtk_box_pack_start(g_box_buttons, g_btn_repeat, false, false, 5);
    /* Btns above are to the left, the rest are to the right: */
#if ((GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 13) || (GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION == 13 && GTK_MICRO_VERSION < 5))
    GtkWidget *w = gtk_alignment_new(0.0, 0.0, 1.0, 1.0);
    gtk_box_pack_start(g_box_buttons, w, true, true, 5);
    gtk_box_pack_start(g_box_buttons, g_btn_detail, false, false, 5);
    gtk_box_pack_start(g_box_buttons, g_btn_next, false, false, 5);
#else
    gtk_widget_set_valign(GTK_WIDGET(g_btn_next), GTK_ALIGN_END);
    gtk_box_pack_end(g_box_buttons, g_btn_next, false, false, 5);
    gtk_box_pack_end(g_box_buttons, g_btn_detail, false, false, 5);
#endif

    {   /* Warnings area widget definition start */
        g_box_warning_labels = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
        gtk_widget_set_visible(GTK_WIDGET(g_box_warning_labels), TRUE);

        GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
        gtk_widget_set_visible(GTK_WIDGET(vbox), TRUE);
        gtk_box_pack_start(vbox, GTK_WIDGET(g_box_warning_labels), false, false, 5);

        GtkWidget *image = gtk_image_new_from_icon_name("dialog-warning-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_widget_set_visible(image, TRUE);

        g_widget_warnings_area = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
        gtk_widget_set_visible(g_widget_warnings_area, FALSE);
        gtk_widget_set_no_show_all(g_widget_warnings_area, TRUE);

#if ((GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 13) || (GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION == 13 && GTK_MICRO_VERSION < 5))
        GtkWidget *alignment_left = gtk_alignment_new(0.5,0.5,1,1);
        gtk_widget_set_visible(alignment_left, TRUE);
        gtk_box_pack_start(GTK_BOX(g_widget_warnings_area), alignment_left, true, false, 0);
#else
        gtk_widget_set_valign(GTK_WIDGET(image), GTK_ALIGN_CENTER);
        gtk_widget_set_valign(GTK_WIDGET(vbox), GTK_ALIGN_CENTER);
#endif

        gtk_box_pack_start(GTK_BOX(g_widget_warnings_area), image, false, false, 5);
        gtk_box_pack_start(GTK_BOX(g_widget_warnings_area), GTK_WIDGET(vbox), false, false, 0);

#if ((GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION < 13) || (GTK_MAJOR_VERSION == 3 && GTK_MINOR_VERSION == 13 && GTK_MICRO_VERSION < 5))
        GtkWidget *alignment_right = gtk_alignment_new(0.5,0.5,1,1);
        gtk_widget_set_visible(alignment_right, TRUE);
        gtk_box_pack_start(GTK_BOX(g_widget_warnings_area), alignment_right, true, false, 0);
#endif
    }   /* Warnings area widget definition end */

    g_box_assistant = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_box_pack_start(g_box_assistant, GTK_WIDGET(g_assistant), true, true, 0);

    gtk_box_pack_start(g_box_assistant, GTK_WIDGET(g_widget_warnings_area), false, false, 0);
    gtk_box_pack_start(g_box_assistant, GTK_WIDGET(g_box_buttons), false, false, 5);

    gtk_widget_show_all(GTK_WIDGET(g_box_buttons));
    gtk_widget_hide(g_btn_stop);
    gtk_widget_hide(g_btn_onfail);
    gtk_widget_hide(g_btn_repeat);
    gtk_widget_show(g_btn_next);

    g_wnd_assistant = GTK_WINDOW(gtk_application_window_new(app));
    gtk_container_add(GTK_CONTAINER(g_wnd_assistant), GTK_WIDGET(g_box_assistant));

    gtk_window_set_default_size(g_wnd_assistant, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    /* set_default sets icon for every windows used in this app, so we don't
     * have to set the icon for those windows manually
     */
    gtk_window_set_default_icon_name("abrt");

    init_pages();

    add_pages();

    create_details_treeview();

    ProblemDetailsWidget *details = problem_details_widget_new(g_cd);
    gtk_container_add(GTK_CONTAINER(g_container_details1), GTK_WIDGET(details));

    g_signal_connect(g_btn_close, "clicked", G_CALLBACK(assistant_quit_cb), NULL);
    g_signal_connect(g_btn_stop, "clicked", G_CALLBACK(on_btn_cancel_event), NULL);
    g_signal_connect(g_btn_onfail, "clicked", G_CALLBACK(on_btn_failed_cb), NULL);
    g_signal_connect(g_btn_repeat, "clicked", G_CALLBACK(on_btn_repeat_cb), NULL);
    g_signal_connect(g_btn_next, "clicked", G_CALLBACK(on_next_btn_cb), NULL);

    g_signal_connect(g_wnd_assistant, "destroy", G_CALLBACK(assistant_quit_cb), NULL);
    g_signal_connect(g_assistant, "switch-page", G_CALLBACK(on_page_prepare), NULL);

    g_signal_connect(g_tb_approve_bt, "toggled", G_CALLBACK(on_bt_approve_toggle), NULL);
    g_signal_connect(gtk_text_view_get_buffer(g_tv_comment), "changed", G_CALLBACK(on_comment_changed), NULL);

    g_signal_connect(g_btn_add_file, "clicked", G_CALLBACK(on_btn_add_file), NULL);
    g_signal_connect(g_btn_detail, "clicked", G_CALLBACK(on_btn_detail), NULL);

    if (is_screencast_available()) {
        /* we need to override the activate-link handler, because we use
         * the link button instead of normal button and if we wouldn't override it
         * gtk would try to run it's defualt action and open the associated URI
         * but since the URI is empty it would complain about it...
         */
        g_signal_connect(g_btn_startcast, "activate-link", G_CALLBACK(on_btn_startcast), NULL);
    }
    else {
        gtk_widget_set_sensitive(GTK_WIDGET(g_btn_startcast), false);
        gtk_widget_set_tooltip_markup(GTK_WIDGET(g_btn_startcast),
          _("In order to enable the built-in screencasting "
            "functionality the package fros-recordmydesktop has to be installed. "
            "Please run the following command if you want to install it."
            "\n\n"
            "<b>su -c \"yum install fros-recordmydesktop\"</b>"
            ));
    }


    g_signal_connect(g_search_entry_bt, "changed", G_CALLBACK(search_timeout), NULL);

    g_signal_connect (g_tv_event_log, "key-press-event", G_CALLBACK (key_press_event), NULL);
    g_signal_connect (g_tv_event_log, "event-after", G_CALLBACK (event_after), NULL);
    g_signal_connect (g_tv_event_log, "motion-notify-event", G_CALLBACK (motion_notify_event), NULL);
    g_signal_connect (g_tv_event_log, "visibility-notify-event", G_CALLBACK (visibility_notify_event), NULL);
    g_signal_connect (gtk_text_view_get_buffer(g_tv_event_log), "changed", G_CALLBACK (on_log_changed), NULL);

    hand_cursor = gdk_cursor_new (GDK_HAND2);
    regular_cursor = gdk_cursor_new (GDK_XTERM);

    /* switch to right starting page */
    on_page_prepare(g_assistant, gtk_notebook_get_nth_page(g_assistant, 0), NULL);
}
