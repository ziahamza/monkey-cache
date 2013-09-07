#ifndef __CURR_REQS_H_
#define __CURR_REQS_H_

#include "MKPlugin.h"
#include "cache_req.h"

pthread_key_t curr_reqs;
void curr_reqs_init();
struct cache_req_t *curr_reqs_get(int socket);
void curr_reqs_add(struct cache_req_t *req);
void curr_reqs_del(int socket);
#endif
