/*
 * gobject-list: a LD_PRELOAD library for tracking the lifetime of GObjects
 *
 * Copyright (C) 2011  Collabora Ltd.
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

static GHashTable *objects = NULL;

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
      g_print (" - %p, %s: %u refs\n",
          obj, G_OBJECT_TYPE_NAME (obj), obj->ref_count);
    }
#endif
}

static void
_sig_usr1_handler (int signal)
{
  g_print ("Living Objects:\n");

  _dump_object_list (objects);
}

static void
_exiting (void)
{
  g_print ("\nStill Alive:\n");

  _dump_object_list (objects);
}

static void *
get_func (const char *func_name)
{
  static void *handle = NULL;
  void *func;
  char *error;

  if (G_UNLIKELY (handle == NULL))
    {
      handle = dlopen("libgobject-2.0.so.0", RTLD_LAZY);

      if (handle == NULL)
        g_error ("Failed to open libgobject-2.0.so.0: %s", dlerror ());

      /* set up signal handler */
      signal (SIGUSR1, _sig_usr1_handler);

      /* set up objects map */
      objects = g_hash_table_new (NULL, NULL);

      /* Set up exit handler */
      atexit (_exiting);
    }

  func = dlsym (handle, func_name);

  if ((error = dlerror ()) != NULL)
    g_error ("Failed to find symbol: %s", error);

  return func;
}

static void
_object_finalized (gpointer data,
    GObject *obj)
{
  if (display_filter (DISPLAY_FLAG_CREATE))
    {
      g_print (" -- Finalized object %p, %s\n", obj, G_OBJECT_TYPE_NAME (obj));
      print_trace();
    }

  g_hash_table_remove (objects, obj);
}

gpointer
g_object_new (GType type,
    const char *first,
    ...)
{
  gpointer (* real_g_object_new_valist) (GType, const char *, va_list);
  va_list var_args;
  GObject *obj;
  const char *obj_name;

  real_g_object_new_valist = get_func ("g_object_new_valist");

  va_start (var_args, first);
  obj = real_g_object_new_valist (type, first, var_args);
  va_end (var_args);

  obj_name = G_OBJECT_TYPE_NAME (obj);

  if (g_hash_table_lookup (objects, obj) == NULL &&
      object_filter (obj_name))
    {
      if (display_filter (DISPLAY_FLAG_CREATE))
        {
          g_print (" ++ Created object %p, %s\n", obj, obj_name);
          print_trace();
        }

      g_object_weak_ref (obj, _object_finalized, NULL);

      g_hash_table_insert (objects, obj, GUINT_TO_POINTER (TRUE));
    }

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
