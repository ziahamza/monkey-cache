#ifndef __CURR_REQS_H_
#define __CURR_REQS_H_

#include "MKPlugin.h"
#include "cache_req.h"

void curr_reqs_process_init();
void curr_reqs_thread_init();
void curr_reqs_exit();
struct cache_req_t *curr_reqs_get(int socket);
void curr_reqs_add(struct cache_req_t *req);
void curr_reqs_del(int socket);
#endif
