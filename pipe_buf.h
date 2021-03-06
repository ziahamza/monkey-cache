/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#ifndef __PIPE_BUF_H_
#define __PIPE_BUF_H_

#include <pthread.h>

#include "MKPlugin.h"

struct pipe_buf_t {
    int pipe[2];
    int filled;     // amount of data filled in pipe
    int cap;        // tatal buffer size of pipe (should block afterwards)

    pthread_mutex_t write_mutex; // write access to the buffer

    struct mk_list _head;
};

void pipe_buf_thread_init();
void pipe_buf_process_init();
void pipe_buf_exit();

int pipe_buf_mem_used();

// map userspace memory as pipe data
int pipe_buf_vmsplice(struct pipe_buf_t *dest, void *mem, int len);

struct pipe_buf_t *pipe_buf_new();
void pipe_buf_flush(struct pipe_buf_t *);
void pipe_buf_free(struct pipe_buf_t *);

#endif
