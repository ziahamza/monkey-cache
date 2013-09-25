/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#ifndef __HT_H_
#define __HT_H_

#include <string.h>

// callback takes key, val and updates the state, and returns the new state
// at the end
typedef void * (*table_cb_t)(const char *key, void *val, void *state);

struct table_t *table_alloc();

void table_free(struct table_t *);

void *table_get(struct table_t *, const char *);

// run cb on all the entries
void *table_each(struct table_t *, table_cb_t cb, void *state);

// key string managed by the caller and used till table_del
int table_add(struct table_t *, const char *, void *);

// returns the added object after delete
void *table_del(struct table_t *, const char *);

#endif
