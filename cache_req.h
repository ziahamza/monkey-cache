/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#ifndef __CACHE_REQ_H_
#define __CACHE_REQ_H_

#include "MKPlugin.h"

pthread_key_t cache_req_pool;

struct cache_req_t {
    int socket;

    struct cache_file_t *file;

    // pending file data to be send
    struct pipe_buf_t *curr;
    long bytes_offset, bytes_to_send;


    // pipe buffer for the request
    struct pipe_buf_t *buf;

    struct mk_list _head;
};

void cache_req_process_init();
void cache_req_thread_init();
void cache_req_exit();

struct cache_req_t *cache_req_new();
void cache_req_del(struct cache_req_t *);

#endif
