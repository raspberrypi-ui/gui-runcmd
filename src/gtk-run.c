/*
 * gtk-run.c: Little application launcher
 * Copyright (C) 2006-2010 Hong Jen Yee (PCMan) pcman.tw(AT)gmail.com
 *               2006-2008 Jim Huang <jserv.tw@gmail.com>
 *               2008 Fred Chien <fred@lxde.org>
 *               2009 Ying-Chun Liu (PaulLiu) <grandpaul@gmail.com>
 *               2009-2010 Marty Jack <martyj19@comcast.net>
 *               2012-2013 Henry Gebhardt <hsggebhardt@gmail.com>
 *               2012 Piotr Sipika <Piotr.Sipika@gmail.com>
 *               2014 Andriy Grytsenko <andrej@rep.kiev.ua>
 *
 * This file is a part of LXPanel project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <string.h>
#include <unistd.h>

#include <menu-cache.h>

static GtkWidget* win = NULL; /* the run dialog */
static GtkEntry *entry;
static MenuCache* menu_cache = NULL;
static GSList* app_list = NULL; /* all known apps in menu cache */
static gpointer reload_notify_id = NULL;

typedef struct _ThreadData
{
    gboolean cancel; /* is the loading cancelled */
    GSList* files; /* all executable files found */
    GtkEntry* entry;
}ThreadData;

static ThreadData* thread_data = NULL; /* thread data used to load availble programs in PATH */

void launch_application (const char *appname)
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

static void setup_auto_complete( GtkEntry* entry )
{
    /* load in another working thread */
    thread_data = g_slice_new0(ThreadData); /* the data will be freed in idle handler later. */
    thread_data->entry = entry;
    g_thread_new("gtk-run-autocomplete", (GThreadFunc)thread_func, thread_data);
    /* we don't use loader_thread_id but Glib 2.32 crashes if we unref
       GThread while it's in creation progress. It is a bug of GLib
       certainly but as workaround we'll unref it in the thread itself */
}

static void mc_unref (gpointer data, gpointer)
{
    MenuCacheItem* item = (MenuCacheItem *) data;
    menu_cache_item_unref (item);
}

static void reload_apps(MenuCache* cache, gpointer)
{
    g_debug("reload apps!");
    if(app_list)
    {
        g_slist_foreach(app_list, mc_unref, NULL);
        g_slist_free(app_list);
    }
    app_list = menu_cache_list_all_apps(cache);
}

static void on_response( GtkWidget* dlg, gint response, gpointer user_data )
{
    if( G_LIKELY(response == GTK_RESPONSE_OK) )
    {
        launch_application (gtk_entry_get_text(entry));
    }

    /* cancel running thread if needed */
    if( thread_data ) /* the thread is still running */
        thread_data->cancel = TRUE; /* cancel the thread */

    gtk_widget_destroy( dlg );
    win = NULL;

    /* free app list */
    g_slist_foreach(app_list, mc_unref, NULL);
    g_slist_free(app_list);
    app_list = NULL;

    /* free menu cache */
    menu_cache_remove_reload_notify(menu_cache, reload_notify_id);
    reload_notify_id = NULL;
    menu_cache_unref(menu_cache);
    menu_cache = NULL;

    gtk_main_quit ();
}

static gboolean delete_event (GtkWidget *widget, GdkEvent *event, gpointer data)
{
    on_response (win, GTK_RESPONSE_CANCEL, NULL);
    return FALSE;
}

static void button_handler (GtkWidget *widget, gpointer data)
{
    on_response (win, (int) data, NULL);
}

static void on_entry_changed( GtkEntry* entry, GtkImage* img )
{
    const char* str = gtk_entry_get_text(entry);
    MenuCacheApp* app = NULL;
    if( str && *str )
        app = match_app_by_exec(str);

    if( app )
    {
        const char *name = menu_cache_item_get_icon(MENU_CACHE_ITEM(app));
        if (name)
        {
            gtk_image_set_from_icon_name(img, name, GTK_ICON_SIZE_DND);
            return;
        }
    }
    gtk_image_set_from_icon_name(img, "gtk-execute", GTK_ICON_SIZE_DND);
}

int main (int argc, char *argv[])
{
    GtkWidget *img;
    GtkBuilder *builder;

    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    gtk_init (&argc, &argv);

    builder = gtk_builder_new_from_file (PACKAGE_UI_DIR "/gtk-run.ui");

    win = (GtkWidget *) gtk_builder_get_object (builder, "main_wd");
    entry = (GtkEntry *) gtk_builder_get_object (builder, "entry_cmd");
    img = (GtkWidget *) gtk_builder_get_object (builder, "icon");

    g_signal_connect (G_OBJECT (win), "delete_event", G_CALLBACK (delete_event), NULL);
    g_signal_connect (gtk_builder_get_object (builder, "btn_ok"), "clicked", G_CALLBACK (button_handler), (void *) GTK_RESPONSE_OK);
    g_signal_connect (gtk_builder_get_object (builder, "btn_cancel"), "clicked", G_CALLBACK (button_handler), (void *) GTK_RESPONSE_CANCEL);
    g_signal_connect(entry ,"changed", G_CALLBACK(on_entry_changed), img);

    gtk_widget_show_all( win );

    setup_auto_complete( (GtkEntry*)entry );

    /* get all apps */
    menu_cache = menu_cache_lookup_sync(g_getenv("XDG_MENU_PREFIX") ? "applications.menu" : "lxde-applications.menu" );
    if( menu_cache )
    {
        app_list = menu_cache_list_all_apps(menu_cache);
        reload_notify_id = menu_cache_add_reload_notify(menu_cache, reload_apps, NULL);
    }

    gtk_window_present(GTK_WINDOW(win));
    gtk_main ();
    return 0;
}
