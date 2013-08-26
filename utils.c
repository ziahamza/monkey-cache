#include <stdlib.h>
#include <stdio.h>

#include "utils.h"

void buf_init(struct buf_t *buf, int init_size) {
    buf->cap = init_size;
    buf->data = malloc(init_size);
    buf->size = 0;
}
void buf_resize(struct buf_t *buf, int nsize) {
    buf->cap = nsize;
    buf->data = realloc(buf->data, buf->cap);
}

void buf_reset(struct buf_t *buf) {
    buf->size = 0;
}

void buf_push(struct buf_t *buf, const void *item, int size) {
    if (size + buf->size > buf->cap) {
        buf_resize(buf, 2 * (size + buf->size));
    }

    memcpy(buf->data + buf->size, item, size);
    buf->size += size;
}

void buf_free(struct buf_t *buf) {
    free(buf->data);
}


