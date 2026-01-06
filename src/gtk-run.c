/*============================================================================
Copyright (c) 2026 Raspberry Pi
All rights reserved.

Some code taken from the lxpanel project

Copyright (c) 2006-2010 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
            2006-2008 Jim Huang <jserv.tw@gmail.com>
            2008 Fred Chien <fred@lxde.org>
            2009 Ying-Chun Liu (PaulLiu) <grandpaul@gmail.com>
            2009-2010 Marty Jack <martyj19@comcast.net>
            2010 Jürgen Hötzel <juergen@archlinux.org>
            2010-2011 Julien Lavergne <julien.lavergne@gmail.com>
            2012-2013 Henry Gebhardt <hsggebhardt@gmail.com>
            2012 Michael Rawson <michaelrawson76@gmail.com>
            2014 Max Krummenacher <max.oss.09@gmail.com>
            2014 SHiNE CsyFeK <csyfek@users.sourceforge.net>
            2014 Andriy Grytsenko <andrej@rep.kiev.ua>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

No AI tools were used in the creation of this code.
============================================================================*/

#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <menu-cache.h>

/*----------------------------------------------------------------------------*/
/* Typedefs and macros                                                        */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Global data                                                                */
/*----------------------------------------------------------------------------*/

static GtkWidget *win, *entry, *icon;
static MenuCache *menu_cache = NULL;
static GSList *app_list = NULL;
static GtkListStore *path_apps;
static GThread *app_thread = NULL;
static gboolean thread_term = FALSE;

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static void launch_application (const char *appname);
static gpointer find_apps (gpointer user_data);
static gboolean find_apps_done (gpointer user_data);
static void entry_changed_event (GtkEntry* entry, gpointer user_data);
static MenuCacheApp* match_app_by_exec (const char* exec);
static gboolean delete_event (GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void button_handler (GtkWidget *widget, gpointer user_data);
static gboolean key_press_event (GtkWidget *widget, GdkEventKey *event, gpointer user_data);

/*----------------------------------------------------------------------------*/
/* Function definitions                                                       */
/*----------------------------------------------------------------------------*/

/* Application launch */

static void launch_application (const char *appname)
{
    char *cmd[2] = {(char *) appname, NULL};
    g_spawn_async (NULL, cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

/* Scan system path for executable files */

static gpointer find_apps (gpointer user_data)
{
    const char *name;
    char *filename;
    GDir *dir;
    gchar **dirname, **dirnames;
    GtkTreeIter iter;
    GHashTable *hash;

    hash = g_hash_table_new (g_direct_hash, g_direct_equal);
    dirnames = g_strsplit (g_getenv ("PATH"), ":", 0);
    for (dirname = dirnames; *dirname; ++dirname)
    {
        if (thread_term) break;
        dir = g_dir_open (*dirname, 0, NULL);
        if (!dir) continue;
        while ((name = g_dir_read_name (dir)) != NULL)
        {
            if (thread_term) break;
            filename = g_build_filename (*dirname, name, NULL);
            if (g_file_test (filename, G_FILE_TEST_IS_EXECUTABLE))
            {
                if (!g_hash_table_lookup (hash, name))
                {
                    g_hash_table_add (hash, (gpointer) name);
                    gtk_list_store_append (path_apps, &iter);
                    gtk_list_store_set (path_apps, &iter, 0, name, -1);
                }
            }
            g_free (filename);
        }
        g_dir_close (dir);
    }
    g_strfreev (dirnames);
    g_hash_table_unref (hash);

    if (!thread_term) g_idle_add ((GSourceFunc) find_apps_done, NULL);
    g_thread_unref (app_thread);
    app_thread = NULL;

    return NULL;
}

static gboolean find_apps_done (gpointer user_data)
{
    if (thread_term) return FALSE;

    GtkEntryCompletion* comp = gtk_entry_completion_new ();
    gtk_entry_completion_set_minimum_key_length (comp, 1);
    gtk_entry_completion_set_inline_completion (comp, TRUE);
    gtk_entry_completion_set_popup_set_width (comp, TRUE);
    gtk_entry_completion_set_popup_single_match (comp, FALSE);
    gtk_entry_completion_set_model (comp, GTK_TREE_MODEL (path_apps));
    gtk_entry_completion_set_text_column (comp, 0);
    gtk_entry_set_completion (GTK_ENTRY (entry), comp);
    gtk_entry_completion_complete (comp);
    g_object_unref (comp);
    g_object_unref (path_apps);

    return FALSE;
}

/* Handle updating of icon when entry changed */

static void entry_changed_event (GtkEntry* entry, gpointer user_data)
{
    const char *str = gtk_entry_get_text (entry);
    MenuCacheApp *app = NULL;

    if (str && *str) app = match_app_by_exec (str);

    if (app)
    {
        const char *name = menu_cache_item_get_icon (MENU_CACHE_ITEM (app));
        if (name)
        {
            gtk_image_set_from_icon_name (GTK_IMAGE (icon), name, GTK_ICON_SIZE_DIALOG);
            return;
        }
    }
    gtk_image_set_from_icon_name (GTK_IMAGE (icon), "gtk-execute", GTK_ICON_SIZE_DIALOG);
}

static MenuCacheApp *match_app_by_exec (const char* exec)
{
    GSList *l;
    MenuCacheApp *app, *ret = NULL;
    char *exec_path;
    const char *pexec, *app_exec;
    int len;

    exec_path = g_find_program_in_path (exec);
    if (!exec_path) return NULL;

    for (l = app_list; l; l = l->next)
    {
        app = MENU_CACHE_APP (l->data);
        app_exec = menu_cache_app_get_exec (app);
        if (!app_exec) continue;

        if (g_path_is_absolute (app_exec)) pexec = exec_path;
        else pexec = exec;
        len = strlen (pexec);

        if (strncmp (app_exec, pexec, len) == 0)
        {
            /* exact match has the highest priority */
            if (app_exec[len] == 0)
            {
                ret = app;
                break;
            }

            /* those matches the pattern: exe_name %F|%f|%U|%u have higher priority */
            if (app_exec[len] == ' ')
            {
                ret = app;
                if (app_exec[len + 1] == '%' && strchr ("FfUu", app_exec[len + 2])) break;
            }
        }
    }

    /* if this is a symlink */
    if (!ret && g_file_test (exec_path, G_FILE_TEST_IS_SYMLINK))
    {
        char target[512]; /* FIXME: is this enough? */
        len = readlink (exec_path, target, sizeof (target) - 1);
        if (len > 0)
        {
            target[len] = '\0';

            char *sympath = g_canonicalize_filename (target, g_path_get_dirname (exec_path));
            char *basename = g_path_get_basename (sympath);
            ret = match_app_by_exec (basename);
            g_free (sympath);
            g_free (basename);
        }
    }

    g_free (exec_path);
    return ret;
}

/* UI handlers */

static gboolean delete_event (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    win = NULL;
    gtk_main_quit ();
    return FALSE;
}

static void button_handler (GtkWidget *widget, gpointer user_data)
{
    if (user_data) launch_application (gtk_entry_get_text (GTK_ENTRY (entry)));
    gtk_main_quit ();
}

static gboolean key_press_event (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if (event->keyval == GDK_KEY_Escape)
    {
        gtk_main_quit ();
        return TRUE;
    }
    return FALSE;
}

/* Main function */

int main (int argc, char *argv[])
{
    GtkBuilder *builder;

    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    gtk_init (&argc, &argv);

    builder = gtk_builder_new_from_file (PACKAGE_UI_DIR "/gui-runcmd.ui");

    win = (GtkWidget *) gtk_builder_get_object (builder, "main_wd");
    entry = (GtkWidget *) gtk_builder_get_object (builder, "entry_cmd");
    icon = (GtkWidget *) gtk_builder_get_object (builder, "icon");

    g_signal_connect (win, "delete_event", G_CALLBACK (delete_event), NULL);
    g_signal_connect (win, "key-press-event", G_CALLBACK (key_press_event), NULL);
    g_signal_connect (entry ,"changed", G_CALLBACK (entry_changed_event), NULL);
    g_signal_connect (gtk_builder_get_object (builder, "btn_ok"), "clicked", G_CALLBACK (button_handler), (void *) 1);
    g_signal_connect (gtk_builder_get_object (builder, "btn_cancel"), "clicked", G_CALLBACK (button_handler), NULL);

    g_object_unref (builder);

    path_apps = gtk_list_store_new (1, G_TYPE_STRING);
    app_thread = g_thread_new (NULL, (GThreadFunc) find_apps, NULL);

    menu_cache = menu_cache_lookup_sync (g_getenv ("XDG_MENU_PREFIX") ? "applications.menu" : "lxde-applications.menu" );
    if (menu_cache) app_list = menu_cache_list_all_apps (menu_cache);

    gtk_widget_show_all (win);
    gtk_window_present (GTK_WINDOW (win));

    gtk_main ();

    if (win) gtk_widget_destroy (win);

    if (app_thread)
    {
        thread_term = TRUE;
        while (app_thread);
    }

    if (app_list)
    {
        g_slist_foreach (app_list, (GFunc) menu_cache_item_unref, NULL);
        g_slist_free (app_list);
    }

    if (menu_cache) menu_cache_unref (menu_cache);

    return 0;
}

/* End of file */
/*----------------------------------------------------------------------------*/
