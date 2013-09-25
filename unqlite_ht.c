/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#include <stdlib.h>
#include  <stdint.h>

#include "unqlite.h"
#include "ht.h"

#include "MKPlugin.h"

struct node_t {
  void *data;
  int len;
};

struct table_t {
	unqlite *pDb;               /* Database handle */
};


struct table_t * table_alloc() {
  struct table_t *table = malloc(sizeof(struct table_t));
  int rc = unqlite_open(&table->pDb, ":mem:",UNQLITE_OPEN_CREATE);
  mk_bug(rc != UNQLITE_OK );

  return table;
}

void table_free(struct table_t *table) {
  unqlite_close(table->pDb);
  free(table);
}

void *table_each(struct table_t *table, table_cb_t cb, void *state) {

    struct node_t node;
    unqlite_kv_cursor *pCur;    /* Cursor handle */

    mk_bug(unqlite_kv_cursor_init(table->pDb, &pCur) != UNQLITE_OK);

    unqlite_kv_cursor_first_entry(pCur);
    int len = 1024;
    char key[len];


    while( unqlite_kv_cursor_valid_entry(pCur) ){

      unqlite_int64 data_len = sizeof(node);

      unqlite_kv_cursor_key(pCur, key, &len);
      unqlite_kv_cursor_data(pCur, &node, &data_len);

      unqlite_kv_cursor_next_entry(pCur);

      state = cb(key, node.data, state);
    }

    unqlite_kv_cursor_release(table->pDb, pCur);

    return state;
}

void * table_get(struct table_t *table, const char *key) {
  struct node_t node;
  unqlite_int64 data_len = sizeof(node);

  if (unqlite_kv_fetch(table->pDb, key, -1, &node, &data_len) != UNQLITE_OK) {
    return NULL;
  }

  return node.data;
}

void table_add(struct table_t *table, const char *key, void *val) {
  struct node_t node;
  node.data = val;

  mk_bug(unqlite_kv_store(table->pDb, key, -1, &node, sizeof(node)) != UNQLITE_OK);
}

void *table_del(struct table_t *table, const char *key) {
  void *data = table_get(table, key);
  unqlite_kv_delete(table->pDb, key,-1);
  return data;
}

