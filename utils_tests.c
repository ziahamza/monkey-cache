/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */
#include "test.h"

#include "utils.h"

void util_tests() {
    struct buf_t tmp;

    buf_init(&tmp, sizeof(int), 2);
    for (int i = 0; i < 200; i++) {
        buf_push(&tmp, &i);
    }

    for (int i = 0; i < tmp.size; i++) {
        if (err(i != *(int *) buf_get(&tmp, i), "items corrupted in buf_t")) {
            break;
        }
    }
}
