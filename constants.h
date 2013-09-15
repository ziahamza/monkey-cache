/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#ifndef __CONSTANTS_H_
#define __CONSTANTS_H_

#include <sys/mman.h>
#define MMAP_SIZE sysconf(_SC_PAGE_SIZE)


#define API_PREFIX "/cache"
#define API_PREFIX_LEN 6

#define MAX_PATH_LEN 1024
#define MAX_URI_LEN 512

// size of a single pipe
#define PIPE_SIZE 512 * 1024

// decrease both down till zero to minimum

// memory consumption
#define PIPE_BUF_POOL_MAX 16
// although, cache req pool will only contain
// one pool item under any load ?!
#define CACHE_REQ_POOL_MAX 32

#endif
