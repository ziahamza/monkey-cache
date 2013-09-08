/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#include "cache_req.h"
#include "cache_file.h"
#include "pipe_buf.h"
#include <utils.h>

#include "constants.h"

void cache_req_process_init() {
    pthread_key_create(&cache_req_pool, NULL);
}
void cache_req_thread_init() {
    struct mk_list *pool = malloc(sizeof(struct mk_list));
    mk_list_init(pool);
    pthread_setspecific(cache_req_pool, pool);
}

void cache_req_exit() {
  // TODO: release all resources
}

struct cache_req_t *cache_req_new() {
  struct mk_list *pool = pthread_getspecific(cache_req_pool);
  struct cache_req_t *req;

    if (mk_list_is_empty(pool) == -1) {
        // printf("reusing an exhisting request!\n");
        req = mk_list_entry_first(pool, struct cache_req_t, _head);

        mk_list_del(&req->_head);

        if (req->buf->filled) {
            pipe_buf_flush(req->buf);
        }

    }
    else {
        req = malloc(sizeof(struct cache_req_t));
        req->buf = pipe_buf_new();
    }

    req->socket = -1;
    //mk_info("creating a request!");

    mk_bug(req->buf->filled != 0);
    return req;
}

int cache_req_fill_curr(struct cache_req_t *req) {
    struct pipe_buf_t *curr = req->curr;
    struct cache_file_t *file = req->file;

    // offset of the file before the start of filled cache
    long off = req->bytes_offset + req->buf->filled;

    // total amount of data that can be filled from file to cache
    long len = curr->cap;

    if (off + len > file->size) {
        //  this should be the last portion of file
        len = file->size - off;
        /*
        if (len > 0)
            printf("hopefully the last portion of file "
                "(namely %ld bytes) can be filled!\n",
                len);
        */
    }

    mk_bug(off > file->size);
    mk_bug(len < 0);

    if (curr->filled == len) return 0;

    if (curr->filled < len) {

        if (pipe_buf_vmsplice(curr, file->buf.data + curr->filled + off, len - curr->filled) < 0) {

            printf("Tried to write from %ld (out of %ld) till "
                "%ld\n", off, file->size, off + len);
        }
    }

    // mk_bug(curr->filled > len);

    return len - curr->filled;
}

void cache_req_del(struct cache_req_t *req) {
    mk_list_del(&req->_head);

    __sync_fetch_and_add(&req->file->pending_reqs, -1);

    if (req->file->zombie && req->file->pending_reqs == 0) {
        cache_file_free(req->file);
    }
    struct mk_list *pool = pthread_getspecific(cache_req_pool);

    if (mk_list_len(pool) < CACHE_REQ_POOL_MAX) {
        // reuse the request as pipe creation can be expensive
        mk_list_add(&req->_head, pthread_getspecific(cache_req_pool));
    }
    else {
        pipe_buf_free(req->buf);
        free(req);
    }
}
