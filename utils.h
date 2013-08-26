#ifndef __UTILS_H_
#define __UTILS_H_

#include <stdlib.h>
#include <string.h>

struct buf_t {
  void *data;
  int cap;
  int size;
};

void buf_init(struct buf_t *buf, int init_size);
void buf_free(struct buf_t *buf);

void buf_resize(struct buf_t *buf, int size);
void buf_reset(struct buf_t *buf);
void buf_push(struct buf_t *buf, const void *item, int size);

#endif
