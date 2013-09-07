/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#include <stdlib.h>
#include <stdio.h>

#include "utils.h"

void buf_init(struct buf_t *buf, int value_size, int init_size) {
    buf->cap = init_size;
    buf->value_size = value_size;
    buf->data = (void *) -1;
    if (init_size > 0)
        buf->data = malloc(value_size * init_size);

    buf->size = 0;
}
void buf_resize(struct buf_t *buf, int nsize) {
    buf->cap = nsize;
    if (buf->data != (void *) -1)
        buf->data = realloc(buf->data, buf->cap * buf->value_size);
    else
        buf->data = malloc(buf->cap * buf->value_size);
}

void buf_compact(struct buf_t *buf) {
    buf_resize(buf, buf->size);
}

void buf_reset(struct buf_t *buf) {
    buf->size = 0;
}

void buf_push(struct buf_t *buf, const void *item) {
    if (buf->size >= buf->cap) {
        buf_resize(buf, 2 * buf->size);
    }

    buf_set(buf, buf->size, item);
    buf->size += 1;
}

void *buf_get(struct buf_t *buf, int index) {
    return buf->data + (index * buf->value_size);
}

void *buf_set(struct buf_t *buf, int index, const void *val) {
    memcpy(buf->data + (index * buf->value_size), val, buf->value_size);
    return buf->data + (index * buf->value_size);
}

void buf_free(struct buf_t *buf) {
    free(buf->data);
    buf->data = (void *) -1;
    buf->cap = 0;
    buf->size = 0;
}


int mk_list_len(struct mk_list *list) {
    int len = 0;
    struct mk_list *curr;
    mk_list_foreach(curr, list) {
       len++;
    }

    return len;
}
