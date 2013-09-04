/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "pipe_buf.h"
#include "utils.h"

#include "constants.h"

int devnull;
int createpipe(int *fds) {
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        perror("cannot create a pipe!");
        mk_bug(1);
    }

    // optimisation on linux, increase pipe size
    if (fcntl(fds[1], F_SETPIPE_SZ, PIPE_SIZE) < 0) {
        perror("changing pipe size");
        mk_bug(1);
    }

    return PIPE_SIZE;
}

void closepipe(int *fds) {
    close(fds[0]);
    close(fds[1]);
}

void pipe_buf_process_init() {
    devnull = open("/dev/null", O_WRONLY);
}
void pipe_buf_thread_init() {
    struct mk_list *pool = mk_api->mem_alloc(sizeof(struct mk_list));
    mk_list_init(pool);
    pthread_setspecific(pipe_buf_pool, pool);
}

void pipe_buf_exit() {
  // TODO: release resources
}

void pipe_buf_flush(struct pipe_buf_t *buf) {
    printf("flushing what ever is left of pipe: %d\n", buf->filled);
    int res = splice(buf->pipe[0], NULL, devnull, NULL, buf->filled, SPLICE_F_MOVE);
    if (res < 0) {
        perror("cannot flush pipe!!!\n");
        mk_bug(1);
    }
    buf->filled -= res;

    mk_bug(buf->filled != 0);
}

struct pipe_buf_t *pipe_buf_new() {
    struct mk_list *pool = pthread_getspecific(pipe_buf_pool);
    struct pipe_buf_t *buf;
    if (mk_list_is_empty(pool) == -1) {
        buf = mk_list_entry_first(pool, struct pipe_buf_t, _head);
        mk_list_del(&buf->_head);

        if (buf->filled) {
            pipe_buf_flush(buf);
        }

    }
    else {
        buf = malloc(sizeof(struct pipe_buf_t));

        buf->filled = 0;
        buf->cap = createpipe(buf->pipe);

        pthread_mutex_init(&buf->write_mutex, NULL);
    }

    mk_bug(buf->filled != 0);
    return buf;
}

void pipe_buf_free(struct pipe_buf_t *buf) {
    struct mk_list *pool = pthread_getspecific(pipe_buf_pool);
    if (mk_list_len(pool) < PIPE_BUF_POOL_MAX)
        mk_list_add(&buf->_head, pool);
    else {
        closepipe(buf->pipe);
        mk_api->mem_free(buf);
    }
}
