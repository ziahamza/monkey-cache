/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#include <stdlib.h>
#include  <stdint.h>
#include "ht.h"
#include <pthread.h>

#define HT_MIN_SIZE 8192

pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
struct node_t {
  // provided by the caller
  const char *key;
  void *val;

  struct node_t *next;
};

struct table_t {
  struct node_t **store;
  int size;
};



// see: http://stackoverflow.com/questions/2351087/what-is-the-best-32bit-hash-function-for-short-strings-tag-names
size_t key_hash(const char *key, size_t max) {
  unsigned int h;
  unsigned char *p;

  h = 0;
  for (p = (unsigned char*) key; *p != '\0'; p++)
    h = 37 * h + *p;

  return h % max;
}


struct table_t * table_alloc() {
  struct table_t *table = malloc(sizeof(struct table_t));

  table->size = HT_MIN_SIZE;
  table->store = calloc(HT_MIN_SIZE, sizeof(struct node *));

  return table;
}

void table_free(struct table_t *table) {
  struct node_t *node;
  int i;
  for (i = 0; i < table->size; i++) {
    struct node_t *next = node;
    for (
      node = table->store[i];
      node != NULL;
      node = next
    ) {
      next = node->next;
      free(node);
    }
  }

  free(table->store);
  free(table);
}

void *table_each(struct table_t *table, table_cb_t cb, void *state) {
    struct node_t *node;
    for (int i = 0; i < table->size; i++) {
        struct node_t *next;
        for (
          node = table->store[i];
          node != NULL;
          node = next
        ) {
            next = node->next;
            state = cb(node->key, node->val, state);
        }
    }

    return state;
}

void * table_get(struct table_t *table, const char *key) {
  struct node_t *node;
  size_t hash = key_hash(key, table->size);
  for (node = table->store[hash]; node; node = node->next) {

    // optimised for case when same str pointer is passed
    if (node->key == key || strcmp(node->key, key) == 0) {
      // TODO: optisize by pushing node to the start of the list
      return node->val;
    }
  }

  return NULL;
}

int  table_add(struct table_t *table, const char *key, void *val) {

  pthread_mutex_lock(&table_mutex);
  size_t hash = key_hash(key, table->size);

  struct node_t *node = calloc(1, sizeof(struct node_t));
  node->key = key;
  node->val = val;
  node->next = table->store[hash];

  table->store[hash] = node;
  pthread_mutex_unlock(&table_mutex);
  return 0;
}

void *table_del(struct table_t *table, const char *key) {
  pthread_mutex_lock(&table_mutex);
  size_t hash = key_hash(key, table->size);
  struct node_t *node = table->store[hash], *prev;
  void *val = NULL;

  if (!node) return NULL;
  const char *nkey = node->key;
  if (nkey == key || strcmp(nkey, key) == 0) {
    table->store[hash] = node->next;
    val = node->val;
    free(node);
  }
  else {
    for (prev = node, node = node->next; node; prev = node, node = node->next) {
      if (node->key == key || strcmp(node->key, key)) {
        prev->next = node->next;
        val = node->val;
        free(node);
        break;
      }
    }
  }

  pthread_mutex_unlock(&table_mutex);
  return val;
}

