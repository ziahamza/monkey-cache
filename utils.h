/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#ifndef __UTILS_H_
#define __UTILS_H_

#include <stdlib.h>
#include <string.h>
#include "MKPlugin.h"

struct buf_t {
  void *data;
  // size of an individual value
  int value_size;

  // total values added in buf
  int size;

  // total amount of values that it can hold
  // before resize
  int cap;
};

// initializes a newly created buf, with data type size and number of data items
void buf_init(struct buf_t *buf, int value_size, int init_size);
void buf_free(struct buf_t *buf);

// resize buf to the size, rest of the memory is released
void buf_resize(struct buf_t *buf, int size);

// move the current write index to the start, total cap stays the same
void buf_reset(struct buf_t *buf);

// free unused memory, may not be efficient if further pushes are expected
void buf_compact(struct buf_t *buf);

// add an item at write index, it literally copies themem to buf using
// the initial init_size, if no memory is available then buf is resized
void buf_push(struct buf_t *buf, const void *item);
void *buf_get(struct buf_t *buf, int index);
void *buf_set(struct buf_t *buf, int index, const void *);


int mk_list_len(struct mk_list *);


#endif
