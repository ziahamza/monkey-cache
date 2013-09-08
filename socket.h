#ifndef __SOCKET_H_
#define __SOCKET_H_

#include "cache_req.h"

// serve the request using just the fd of the file
int socket_serve_req_fd(struct cache_req_t *req);

// a more advance sending method with zero copy using
// pipes, tee and splice
int socket_serve_req_splice(struct cache_req_t *req);
#endif
