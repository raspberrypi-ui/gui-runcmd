/*============================================================================
Copyright (c) 2026 Raspberry Pi
All rights reserved.
No AI tools were used in the creation of this code.

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

typedef struct
{
    gboolean cancel;    /* is the loading cancelled */
    GSList* files;      /* all executable files found */
    GtkEntry* entry;
} ThreadData;

/*----------------------------------------------------------------------------*/
/* Global data                                                                */
/*----------------------------------------------------------------------------*/

static GtkWidget *win, *entry, *icon;
static MenuCache* menu_cache = NULL;
static GSList* app_list = NULL;             /* all known apps in menu cache */
static gpointer reload_notify_id = NULL;
static ThreadData* thread_data = NULL;      /* thread data used to load available programs in PATH */

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Function definitions                                                       */
/*----------------------------------------------------------------------------*/

static void launch_application (const char *appname)
{
    char *cmd[2] = {(char *) appname, NULL};
    g_spawn_async (NULL, cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static MenuCacheApp* match_app_by_exec(const char* exec)
{
    GSList* l;
    MenuCacheApp* ret = NULL;
    char* exec_path = g_find_program_in_path(exec);
    const char* pexec;
    int path_len, exec_len, len;

    if( ! exec_path )
        return NULL;

    path_len = strlen(exec_path);
    exec_len = strlen(exec);

    for( l = app_list; l; l = l->next )
    {
        MenuCacheApp* app = MENU_CACHE_APP(l->data);
        const char* app_exec = menu_cache_app_get_exec(app);
        if ( ! app_exec)
            continue;

        if( g_path_is_absolute(app_exec) )
        {
            pexec = exec_path;
            len = path_len;
        }
        else
        {
            pexec = exec;
            len = exec_len;
        }

        if( strncmp(app_exec, pexec, len) == 0 )
        {
            /* exact match has the highest priority */
            if( app_exec[len] == '\0' )
            {
                ret = app;
                break;
            }
            /* those matches the pattern: exe_name %F|%f|%U|%u have higher priority */
            if( app_exec[len] == ' ' )
            {
                if( app_exec[len + 1] == '%' )
                {
                    if( strchr( "FfUu", app_exec[len + 2] ) )
                    {
                        ret = app;
                        break;
                    }
                }
                ret = app;
            }
        }
    }

    /* if this is a symlink */
    if( ! ret && g_file_test(exec_path, G_FILE_TEST_IS_SYMLINK) )
    {
        char target[512]; /* FIXME: is this enough? */
        len = readlink( exec_path, target, sizeof(target) - 1);
        if( len > 0 )
        {
            target[len] = '\0';
            ret = match_app_by_exec(target);
            if( ! ret )
            {
                /* FIXME: Actually, target could be relative paths.
                 *        So, actually path resolution is needed here. */
                char* basename = g_path_get_basename(target);
                char* locate = g_find_program_in_path(basename);
                if( locate && strcmp(locate, target) == 0 )
                {
                    ret = match_app_by_exec(basename);
                    g_free(locate);
                }
                g_free(basename);
            }
        }
    }

    g_free(exec_path);
    return ret;
}

static void setup_auto_complete_with_data(ThreadData* data)
{
    GtkListStore* store;
    GSList *l;
    GtkEntryCompletion* comp = gtk_entry_completion_new();
    gtk_entry_completion_set_minimum_key_length( comp, 2 );
    gtk_entry_completion_set_inline_completion( comp, TRUE );
    gtk_entry_completion_set_popup_set_width( comp, TRUE );
    gtk_entry_completion_set_popup_single_match( comp, FALSE );
    store = gtk_list_store_new( 1, G_TYPE_STRING );

    for( l = data->files; l; l = l->next )
    {
        const char *name = (const char*)l->data;
        GtkTreeIter it;
        gtk_list_store_append( store, &it );
        gtk_list_store_set( store, &it, 0, name, -1 );
    }

    gtk_entry_completion_set_model( comp, (GtkTreeModel*)store );
    g_object_unref( store );
    gtk_entry_completion_set_text_column( comp, 0 );
    gtk_entry_set_completion( (GtkEntry*)data->entry, comp );

    /* trigger entry completion */
    gtk_entry_completion_complete(comp);
    g_object_unref( comp );
}

static void file_free (gpointer data, gpointer)
{
    g_free (data);
}

static void thread_data_free(ThreadData* data)
{
    g_slist_foreach(data->files, file_free, NULL);
    g_slist_free(data->files);
    g_slice_free(ThreadData, data);
}

static gboolean on_thread_finished(ThreadData* data)
{
    /* don't setup entry completion if the thread is already cancelled. */
    if( !data->cancel )
        setup_auto_complete_with_data(thread_data);
    thread_data_free(data);
    thread_data = NULL; /* global thread_data pointer */
    return FALSE;
}

static gpointer thread_func(ThreadData* data)
{
    GSList *list = NULL;
    gchar **dirname;
    gchar **dirnames = g_strsplit( g_getenv("PATH"), ":", 0 );

    for( dirname = dirnames; !thread_data->cancel && *dirname; ++dirname )
    {
        GDir *dir = g_dir_open( *dirname, 0, NULL );
        const char *name;
        if( ! dir )
            continue;
        while( !thread_data->cancel && (name = g_dir_read_name(dir)) )
        {
            char* filename = g_build_filename( *dirname, name, NULL );
            if( g_file_test( filename, G_FILE_TEST_IS_EXECUTABLE ) )
            {
                if(thread_data->cancel)
                    break;
                if( !g_slist_find_custom( list, name, (GCompareFunc)strcmp ) )
                    list = g_slist_prepend( list, g_strdup( name ) );
            }
            g_free( filename );
        }
        g_dir_close( dir );
    }
    g_strfreev( dirnames );

    data->files = list;
    /* install an idle handler to free associated data */
    g_idle_add((GSourceFunc)on_thread_finished, data);
    g_thread_unref(g_thread_self());

    return NULL;
}

static void setup_auto_complete (void)
{
    /* load in another working thread */
    thread_data = g_slice_new0(ThreadData); /* the data will be freed in idle handler later. */
    thread_data->entry = GTK_ENTRY (entry);
    g_thread_new("gtk-run-autocomplete", (GThreadFunc)thread_func, thread_data);
    /* we don't use loader_thread_id but Glib 2.32 crashes if we unref
       GThread while it's in creation progress. It is a bug of GLib
       certainly but as workaround we'll unref it in the thread itself */
}

static void mc_unref (gpointer data, gpointer user_data)
{
    MenuCacheItem *item = (MenuCacheItem *) data;
    menu_cache_item_unref (item);
}

static void reload_apps (MenuCache* cache, gpointer user_data)
{
    if (app_list)
    {
        g_slist_foreach (app_list, mc_unref, NULL);
        g_slist_free (app_list);
    }
    app_list = menu_cache_list_all_apps (cache);
}

static void on_entry_changed (GtkEntry* entry, gpointer user_data)
{
    const char *str = gtk_entry_get_text (entry);
    MenuCacheApp *app = NULL;

    if (str && *str) app = match_app_by_exec (str);

    if (app)
    {
        const char *name = menu_cache_item_get_icon (MENU_CACHE_ITEM (app));
        if (name)
        {
            gtk_image_set_from_icon_name (GTK_IMAGE (icon), name, GTK_ICON_SIZE_DND);
            return;
        }
    }
    gtk_image_set_from_icon_name (GTK_IMAGE (icon), "gtk-execute", GTK_ICON_SIZE_DND);
}

/* UI handlers */

static gboolean delete_event (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
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

    builder = gtk_builder_new_from_file (PACKAGE_UI_DIR "/gui-runner.ui");

    win = (GtkWidget *) gtk_builder_get_object (builder, "main_wd");
    entry = (GtkWidget *) gtk_builder_get_object (builder, "entry_cmd");
    icon = (GtkWidget *) gtk_builder_get_object (builder, "icon");

    g_signal_connect (win, "delete_event", G_CALLBACK (delete_event), NULL);
    g_signal_connect (win, "key-press-event", G_CALLBACK (key_press_event), NULL);
    g_signal_connect (entry ,"changed", G_CALLBACK (on_entry_changed), NULL);
    g_signal_connect (gtk_builder_get_object (builder, "btn_ok"), "clicked", G_CALLBACK (button_handler), (void *) 1);
    g_signal_connect (gtk_builder_get_object (builder, "btn_cancel"), "clicked", G_CALLBACK (button_handler), NULL);

    g_object_unref (builder);

    setup_auto_complete ();

    /* get all apps */
    menu_cache = menu_cache_lookup_sync (g_getenv ("XDG_MENU_PREFIX") ? "applications.menu" : "lxde-applications.menu" );
    if (menu_cache)
    {
        app_list = menu_cache_list_all_apps (menu_cache);
        reload_notify_id = menu_cache_add_reload_notify (menu_cache, reload_apps, NULL);
    }

    gtk_widget_show_all (win);
    gtk_window_present (GTK_WINDOW (win));

    gtk_main ();

    /* cancel running thread if needed */
    if (thread_data) thread_data->cancel = TRUE;

    gtk_widget_destroy (win);

    /* free app list */
    if (app_list)
    {
        g_slist_foreach (app_list, mc_unref, NULL);
        g_slist_free (app_list);
    }

    /* free menu cache */
    if (reload_notify_id) menu_cache_remove_reload_notify (menu_cache, reload_notify_id);
    if (menu_cache) menu_cache_unref (menu_cache);

    return 0;
}

/* End of file */
/*----------------------------------------------------------------------------*/
