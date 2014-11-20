
typedef struct BtTrie BtTrie;

BtTrie  *bt_create (char *label);
void     bt_free (BtTrie *bt_trie);
void     bt_insert (BtTrie *root, const GPtrArray *items, gboolean is_ref);
void     bt_print_tree (BtTrie *root, guint indent);
