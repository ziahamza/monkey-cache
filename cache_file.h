/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#ifndef __CACHE_FILE_H_
#define __CACHE_FILE_H_

#include "ht.h"
#include "constants.h"
#include <pthread.h>

struct table_t *file_table;

struct cache_file_t {
    // open fd of the file, -1 if fd not available
    int fd;

    // should be mmap in case fd is set, otherwise normal buf
    // if mmap then munmap for cleanup, free in case of normal buf
    // note that buf.len >= size due to alignment
    mk_pointer buf;

    off_t size;

    // hashed based on uri's, assuming mostly unique uri per file
    // NOTE: previously it was based on inodes, but that made it
    // hard to add custom buffer files in that scheme
    char uri[MAX_URI_LEN];

    // list of pipe_buf_t
    struct mk_list cache;

    // cached headers, along with some duplicate file content from cache
    // header len is the length of the headers in the pipe
    struct pipe_buf_t *cache_headers;

    // length of the headers, rest is file contents
    long header_len;

    // file is zombie when it is removed from files table, but some pending
    // requests still need it, when they go to zero, it is freed and went to
    // heaven
    int zombie;
    int pending_reqs;
};

void cache_file_process_init();
void cache_file_thread_init();
void cache_file_exit();

void cache_file_free(struct cache_file_t *);
struct cache_file_t *cache_file_get(const char *);

void cache_file_reset(const char *);
struct cache_file_t *cache_file_tmp(const char *, mk_pointer *);
struct cache_file_t *cache_file_new(const char *, const char *);

#endif
