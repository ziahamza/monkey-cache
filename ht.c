#include <stdlib.h>
#include  <stdint.h>

#define HT_MIN_SIZE 8192

struct node_t {
  ino_t key;
  void *val;

  struct node_t *next;
};


struct table_t {
  struct node_t **store;
  int size;
};

size_t key_hash(ino_t key, size_t max) {
  return ((uint32_t) key * 2654435761U) % max;
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
    struct node_t *next;
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

void * table_get(struct table_t *table, ino_t key) {
  struct node_t *node;
  size_t hash = key_hash(key, table->size);
  for (node = table->store[hash]; node; node = node->next) {
    if (node->key == key) {
      return node->val;
    }
  }

  return NULL;
}


void table_add(struct table_t *table, ino_t key, void *val) {

  size_t hash = key_hash(key, table->size);

  struct node_t *node = calloc(1, sizeof(struct node_t));
  node->key = key;
  node->val = val;
  node->next = table->store[hash];

  table->store[hash] = node;
}

