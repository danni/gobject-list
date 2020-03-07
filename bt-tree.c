/**
 * Stores a call trace ("backtrace") for later inspection.
 *
 * Copyright (C) 2014  Peter Wu <peter@lekensteyn.nl>
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
 */

/* TODO
 * - faster lookups. The path lookup can surely be optimized (cached?)
 * - This code allocates memory which may or may not be a problem depending on
 *   context.
 */

#include <glib.h>
#include "bt-tree.h"

enum {
    COUNT_REF = 0,
    COUNT_UNREF,

    COUNT_LAST
};

typedef struct BtTrie {
    GHashTable *children;
    char *label;
    unsigned count[COUNT_LAST];
} BtTrie;

BtTrie *
bt_create (char *label)
{
    BtTrie *bt_trie = g_malloc0 (sizeof(BtTrie));
    bt_trie->label = label;
    bt_trie->children = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               NULL, (GDestroyNotify) bt_free);
    return bt_trie;
}

void
bt_free (BtTrie *bt_trie)
{
    g_free (bt_trie->label);
    g_hash_table_unref (bt_trie->children);
    g_free (bt_trie);
}

/* returns the child of bt_trie with the item at position i inserted. The memory
 * is freed if such a child already exists. */
static inline BtTrie *
find_child (BtTrie *bt_trie, const GPtrArray *items, guint i)
{
    BtTrie *child = NULL;
    char *label = g_ptr_array_index (items, i);
    g_hash_table_lookup_extended (bt_trie->children, label,
                                  NULL, (gpointer *)&child);
    if (child == NULL) {
        child = bt_create (label);
        g_hash_table_insert (bt_trie->children, child->label, child);
    } else {
        /* unused label */
        g_free (label);
    }
    return child;
}

/**
 * Inserts a trace described by items into a trie. Memory can be allocated if a
 * node is missing.
 * @bt_trie: root of the tree.
 * @items: the items to insert (in reverse order: the first element is the leaf,
 * the last element is the root). Must not be empty. The control of the contents
 * is transferred from the caller.
 */
void
bt_insert (BtTrie *bt_trie, const GPtrArray *items, gboolean is_ref)
{
    guint i = items->len;
    ++bt_trie->count[is_ref ? COUNT_REF : COUNT_UNREF]; /* mark root */
    while (i-- > 0) {
        bt_trie = find_child (bt_trie, items, i);
        ++bt_trie->count[is_ref ? COUNT_REF : COUNT_UNREF];
    }
}

static void
_bt_print_tree (gpointer key, gpointer value, gpointer user_data)
{
    const char *label = key;
    BtTrie *tree = value;
    guint indent = GPOINTER_TO_INT (user_data), i;
    gint diff = tree->count[COUNT_REF] - tree->count[COUNT_UNREF];
    const char
        *color_default  = "\e[1;34m", /* blue */
        *color_unref    = "\e[0;31m", /* red */
        *color_ref      = "\e[0;33m", /* yellow */
        *color_diff;

    if (diff == 0)      /* not important */
        color_default = color_unref = color_ref = color_diff =
            "\e[1;30m"; /* gray */
    else if (diff < 0)  /* more unrefs than refs */
        color_diff = "\e[1;31m"; /* red */
    else /* diff > 0,      more refs than unrefs */
        color_diff = "\e[1;33m"; /* yellow */

    for (i = 0; i < indent; i++)
        g_print("| ");
    g_print ("%s# %s ", color_default, label); /* name */
    g_print ("("
             "%s+%u%s"      /* refs */
             "/"
             "%s-%u%s"      /* unrefs */
             " = %s%d%s",   /* diff */
             color_ref, tree->count[COUNT_REF], color_default,
             color_unref, tree->count[COUNT_UNREF], color_default,
             color_diff, diff, color_default);
    g_print (")\e[m\n");
    g_hash_table_foreach (tree->children, _bt_print_tree,
                          GINT_TO_POINTER (indent + 1));
}

void
bt_print_tree (BtTrie *root, guint indent)
{
    _bt_print_tree (root->label, root, GINT_TO_POINTER (indent));
}
