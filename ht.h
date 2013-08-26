#ifndef __HT_H_
#define __HT_H_

struct node_t {
  ino_t key;
  void *val;

  struct node_t *next;
};


struct table_t {
  struct node_t **store;
  int size;
};


struct table_t * table_alloc();
void table_free(struct table_t *);

void table_add(struct table_t *, ino_t, void *);
void * table_get(struct table_t *, ino_t);


#endif
