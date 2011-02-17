#include <glib-object.h>

#include <dlfcn.h>
#include <signal.h>

static GHashTable *objects = NULL;

static void
_dump_object_list (int signal)
{
  GHashTableIter iter;
  GObject *obj;

  g_print ("Living Objects:\n");

  g_hash_table_iter_init (&iter, objects);
  while (g_hash_table_iter_next (&iter, (gpointer) &obj, NULL))
    {
      g_print (" - %p, %s: %u refs\n",
          obj, G_OBJECT_TYPE_NAME (obj), obj->ref_count);
    }
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
      signal (SIGUSR1, _dump_object_list);

      /* set up objects map */
      objects = g_hash_table_new (NULL, NULL);
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

  real_g_object_new_valist = get_func ("g_object_new_valist");

  va_start (var_args, first);
  obj = real_g_object_new_valist (type, first, var_args);
  va_end (var_args);

  if (g_hash_table_lookup (objects, obj) == NULL)
    {
      g_print (" ++ Created object %p, %s\n", obj, G_OBJECT_TYPE_NAME (obj));

      g_object_weak_ref (obj, _object_finalized, NULL);

      g_hash_table_insert (objects, obj, GUINT_TO_POINTER (TRUE));
    }

  return obj;
}
