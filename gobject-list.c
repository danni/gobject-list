#include <glib-object.h>

#include <dlfcn.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

static GHashTable *objects = NULL;

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
_dump_object_list (void)
{
  GHashTableIter iter;
  GObject *obj;

  g_hash_table_iter_init (&iter, objects);
  while (g_hash_table_iter_next (&iter, (gpointer) &obj, NULL))
    {
      g_print (" - %p, %s: %u refs\n",
          obj, G_OBJECT_TYPE_NAME (obj), obj->ref_count);
    }
}

static void
_sig_usr1_handler (int signal)
{
  g_print ("Living Objects:\n");

  _dump_object_list ();
}

static void
_exiting (void)
{
  g_print ("\nStill Alive:\n");

  _dump_object_list ();
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
  g_print (" -- Finalized object %p, %s\n", obj, G_OBJECT_TYPE_NAME (obj));
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
      g_print (" ++ Created object %p, %s\n", obj, obj_name);

      g_object_weak_ref (obj, _object_finalized, NULL);

      g_hash_table_insert (objects, obj, GUINT_TO_POINTER (TRUE));
    }

  return obj;
}
