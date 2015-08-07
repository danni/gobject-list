/*
 * gobject-list: a LD_PRELOAD library for tracking the lifetime of GObjects
 *
 * Copyright (C) 2011, 2014  Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 *
 * Authors:
 *     Danielle Madeley  <danielle.madeley@collabora.co.uk>
 *     Philip Withnall  <philip.withnall@collabora.co.uk>
 */
#include <glib-object.h>

#include <dlfcn.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_LIBUNWIND
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

typedef enum
{
  DISPLAY_FLAG_NONE = 0,
  DISPLAY_FLAG_CREATE = 1,
  DISPLAY_FLAG_REFS = 1 << 2,
  DISPLAY_FLAG_BACKTRACE = 1 << 3,
  DISPLAY_FLAG_ALL =
      DISPLAY_FLAG_CREATE | DISPLAY_FLAG_REFS | DISPLAY_FLAG_BACKTRACE,
  DISPLAY_FLAG_DEFAULT = DISPLAY_FLAG_CREATE,
} DisplayFlags;

typedef struct
{
  const gchar *name;
  DisplayFlags flag;
} DisplayFlagsMapItem;

DisplayFlagsMapItem display_flags_map[] =
{
  { "none", DISPLAY_FLAG_NONE },
  { "create", DISPLAY_FLAG_CREATE },
  { "refs", DISPLAY_FLAG_REFS },
  { "backtrace", DISPLAY_FLAG_BACKTRACE },
  { "all", DISPLAY_FLAG_ALL },
};

typedef struct {
  GHashTable *objects;  /* owned */

  /* Those 2 hash tables contains the objects which have been added/removed
   * since the last time we catched the USR2 signal (check point). */
  GHashTable *added;  /* owned */
  /* GObject -> (gchar *) type
   *
   * We keep the string representing the type of the object as we won't be able
   * to get it when displaying later as the object would have been destroyed. */
  GHashTable *removed;  /* owned */
} ObjectData;

/* Global static state, which must be accessed with the @gobject_list mutex
 * held. */
static volatile ObjectData gobject_list_state = { NULL, };

/* Global lock protecting access to @gobject_list_state, since GObject methods
 * may be called from multiple threads concurrently. */
G_LOCK_DEFINE_STATIC (gobject_list);


static gboolean
display_filter (DisplayFlags flags)
{
  static DisplayFlags display_flags = DISPLAY_FLAG_DEFAULT;
  static gboolean parsed = FALSE;

  if (!parsed)
    {
      const gchar *display = g_getenv ("GOBJECT_LIST_DISPLAY");

      if (display != NULL)
        {
          gchar **tokens = g_strsplit (display, ",", 0);
          guint len = g_strv_length (tokens);
          guint i = 0;

          /* If there really are items to parse, clear the default flags */
          if (len > 0)
            display_flags = 0;

          for (; i < len; ++i)
            {
              gchar *token = tokens[i];
              guint j = 0;

              for (; j < G_N_ELEMENTS (display_flags_map); ++j)
                {
                  if (!g_ascii_strcasecmp (token, display_flags_map[j].name))
                    {
                      display_flags |= display_flags_map[j].flag;
                      break;
                    }
                }
            }

          g_strfreev (tokens);
        }

      parsed = TRUE;
    }

  return (display_flags & flags) ? TRUE : FALSE;
}

static gboolean
object_filter (const char *obj_name)
{
  const char *filter = g_getenv ("GOBJECT_LIST_FILTER");

  if (filter == NULL)
    return TRUE;
  else
    return (strncmp (filter, obj_name, strlen (filter)) == 0);
}

static void
print_trace (void)
{
#ifdef HAVE_LIBUNWIND
  unw_context_t uc;
  unw_cursor_t cursor;
  guint stack_num = 0;

  if (!display_filter (DISPLAY_FLAG_BACKTRACE))
    return;

  unw_getcontext (&uc);
  unw_init_local (&cursor, &uc);

  while (unw_step (&cursor) > 0)
    {
      gchar name[129];
      unw_word_t off;
      int result;

      result = unw_get_proc_name (&cursor, name, sizeof (name) - 1, &off);
      if (result < 0 && result != UNW_ENOMEM)
        {
          g_print ("Error getting proc name\n");
          break;
        }

      g_print ("#%d  %s + [0x%08x]\n", stack_num++, name, (unsigned int)off);
    }
}

static void
_dump_object_list (GHashTable *hash)
{
  GHashTableIter iter;
  GObject *obj;

  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer) &obj, NULL))
    {
      /* FIXME: Not really sure how we get to this state. */
      if (obj == NULL || obj->ref_count == 0)
        continue;

      g_print (" - %p, %s: %u refs\n",
          obj, G_OBJECT_TYPE_NAME (obj), obj->ref_count);
    }
  g_print ("%u objects\n", g_hash_table_size (hash));
#endif
}

static void
_sig_usr1_handler (int signal)
{
  g_print ("Living Objects:\n");

  G_LOCK (gobject_list);
  _dump_object_list (gobject_list_state.objects);
  G_UNLOCK (gobject_list);
}

static void
_sig_usr2_handler (int signal)
{
  GHashTableIter iter;
  gpointer obj, type;

  G_LOCK (gobject_list);

  g_print ("Added Objects:\n");
  _dump_object_list (gobject_list_state.added);

  g_print ("\nRemoved Objects:\n");
  g_hash_table_iter_init (&iter, gobject_list_state.removed);
  while (g_hash_table_iter_next (&iter, &obj, &type))
    {
      g_print (" - %p, %s\n", obj, (gchar *) type);
    }
  g_print ("%u objects\n", g_hash_table_size (gobject_list_state.removed));

  g_hash_table_remove_all (gobject_list_state.added);
  g_hash_table_remove_all (gobject_list_state.removed);
  g_print ("\nSaved new check point\n");

  G_UNLOCK (gobject_list);
}

static void
print_still_alive (void)
{
  g_print ("\nStill Alive:\n");

  G_LOCK (gobject_list);
  _dump_object_list (gobject_list_state.objects);
  G_UNLOCK (gobject_list);
}

static void
_exiting (void)
{
  print_still_alive ();
}

/* Handle signals which terminate the process. We’re technically not allowed to
 * call printf() from this signal handler, but we do anyway as it’s only a
 * best-effort debugging tool. */
static void
_sig_bad_handler (int sig_num)
{
  signal (sig_num, SIG_DFL);
  print_still_alive ();
  raise (sig_num);
}

static void *
get_func (const char *func_name)
{
  static void *handle = NULL;
  void *func;
  char *error;

  G_LOCK (gobject_list);

  if (G_UNLIKELY (g_once_init_enter (&handle)))
    {
      void *_handle;

      _handle = dlopen("libgobject-2.0.so.0", RTLD_LAZY);

      if (_handle == NULL)
        g_error ("Failed to open libgobject-2.0.so.0: %s", dlerror ());

      /* set up signal handlers */
      signal (SIGUSR1, _sig_usr1_handler);
      signal (SIGUSR2, _sig_usr2_handler);
      signal (SIGINT, _sig_bad_handler);
      signal (SIGTERM, _sig_bad_handler);
      signal (SIGABRT, _sig_bad_handler);
      signal (SIGSEGV, _sig_bad_handler);

      /* set up objects map */
      gobject_list_state.objects = g_hash_table_new (NULL, NULL);
      gobject_list_state.added = g_hash_table_new (NULL, NULL);
      gobject_list_state.removed = g_hash_table_new_full (NULL, NULL, NULL, g_free);

      /* Set up exit handler */
      atexit (_exiting);

      /* Prevent propagation to child processes. */
      if (g_getenv ("GOBJECT_PROPAGATE_LD_PRELOAD") == NULL)
        {
          g_unsetenv ("LD_PRELOAD");
        }

      g_once_init_leave (&handle, _handle);
    }

  func = dlsym (handle, func_name);

  if ((error = dlerror ()) != NULL)
    g_error ("Failed to find symbol: %s", error);

  G_UNLOCK (gobject_list);

  return func;
}

static void
_object_finalized (gpointer data,
    GObject *obj)
{
  G_LOCK (gobject_list);

  if (display_filter (DISPLAY_FLAG_CREATE))
    {
      g_print (" -- Finalized object %p, %s\n", obj, G_OBJECT_TYPE_NAME (obj));
      print_trace();

      /* Only care about the object which were already existing during last
       * check point. */
      if (g_hash_table_lookup (gobject_list_state.added, obj) == NULL)
        g_hash_table_insert (gobject_list_state.removed, obj,
            g_strdup (G_OBJECT_TYPE_NAME (obj)));
    }

  g_hash_table_remove (gobject_list_state.objects, obj);
  g_hash_table_remove (gobject_list_state.added, obj);

  G_UNLOCK (gobject_list);
}

static void 
_track_object(GObject* obj)
{
    const char* obj_name = G_OBJECT_TYPE_NAME (obj);

    G_LOCK (gobject_list);

    if (g_hash_table_lookup (gobject_list_state.objects, obj) == NULL &&
        object_filter (obj_name))
      {
        if (display_filter (DISPLAY_FLAG_CREATE))
          {
            g_print (" ++ Created object %p, %s\n", obj, obj_name);
            print_trace();
          }

        /* FIXME: For thread safety, GWeakRef should be used here, except it
         * won’t give us notify callbacks. Perhaps an opportunistic combination
         * of GWeakRef and g_object_weak_ref() — the former for safety, the latter
         * for notifications (with the knowledge that due to races, some
         * notifications may get omitted)?
         *
         * Alternatively, we could abuse GToggleRef. Inadvisable because other
         * code could be using it.
         *
         * Alternatively, we could switch to a garbage-collection style of
         * working, where gobject-list runs in its own thread and uses GWeakRefs
         * to keep track of objects. Periodically, it would check the hash table
         * and notify of which references have been nullified. */
        g_object_weak_ref (obj, _object_finalized, NULL);

        g_hash_table_insert (gobject_list_state.objects, obj,
            GUINT_TO_POINTER (TRUE));
        g_hash_table_insert (gobject_list_state.added, obj,
            GUINT_TO_POINTER (TRUE));
      }

    G_UNLOCK (gobject_list);
}

GObject*
g_object_new_valist (GType	  type,
             const gchar *first,
             va_list	  var_args)
{

    gpointer (*real_g_object_new_valist) (GType, const char *, va_list);
    GObject *obj;

    real_g_object_new_valist = get_func ("g_object_new_valist");

    obj = (GObject*)real_g_object_new_valist (type, first, var_args);

    _track_object(obj);
    return obj;
}

gpointer
g_object_new (GType type,
    const char *first,
    ...)
{

  gpointer (*real_g_object_new_valist) (GType, const char *, va_list);
  va_list var_args;
  GObject *obj;

  real_g_object_new_valist = get_func ("g_object_new_valist");

  va_start (var_args, first);
  obj = (GObject*)real_g_object_new_valist (type, first, var_args);
  va_end (var_args);

  _track_object(obj);
  return obj;
}

gpointer
g_object_newv (GType object_type,
               guint n_parameters,
               GParameter *parameters)
{
    GObject *obj;
    gpointer (*real_g_object_newv )(GType object_type, guint n_parameters,GParameter *parameters);

    real_g_object_newv = get_func ("g_object_newv");

    obj = (GObject*)real_g_object_newv(object_type,n_parameters,parameters);
    _track_object(obj);
    return obj;
}

gpointer
g_object_ref (gpointer object)
{
  gpointer (* real_g_object_ref) (gpointer);
  GObject *obj = G_OBJECT (object);
  const char *obj_name;
  guint ref_count;
  GObject *ret;

  real_g_object_ref = get_func ("g_object_ref");

  obj_name = G_OBJECT_TYPE_NAME (obj);

  ref_count = obj->ref_count;
  ret = real_g_object_ref (object);

  if (object_filter (obj_name) && display_filter (DISPLAY_FLAG_REFS))
    {
      g_print (" +  Reffed object %p, %s; ref_count: %d -> %d\n",
          obj, obj_name, ref_count, obj->ref_count);
      print_trace();
    }

  return ret;
}

void
g_object_unref (gpointer object)
{
  void (* real_g_object_unref) (gpointer);
  GObject *obj = G_OBJECT (object);
  const char *obj_name;

  real_g_object_unref = get_func ("g_object_unref");

  obj_name = G_OBJECT_TYPE_NAME (obj);

  if (object_filter (obj_name) && display_filter (DISPLAY_FLAG_REFS))
    {
      g_print (" -  Unreffed object %p, %s; ref_count: %d -> %d\n",
          obj, obj_name, obj->ref_count, obj->ref_count - 1);
      print_trace();
    }

  real_g_object_unref (object);
}
