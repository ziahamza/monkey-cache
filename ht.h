#ifndef __HT_H_
#define __HT_H_

#include <string.h>

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


struct table_t *table_alloc();
void table_free(struct table_t *);

void *table_get(struct table_t *, const char *);

// key string managed by the caller and used till table_del
void table_add(struct table_t *, const char *, void *);
// returns the added object after delete
void *table_del(struct table_t *, const char *);



#endif
