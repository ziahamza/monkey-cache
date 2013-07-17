/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netdb.h>
#include <fcntl.h>

#include "MKPlugin.h"

MONKEY_PLUGIN("cache",         /* shortname */
              "Monkey Cache", /* name */
              VERSION,        /* version */
              MK_PLUGIN_CORE_PRCTX | MK_PLUGIN_CORE_THCTX | MK_PLUGIN_STAGE_50 | MK_PLUGIN_STAGE_40 | MK_PLUGIN_STAGE_30); /* hooks */

pthread_key_t cache_reqs;

struct cache_req_t {
    int socket;
    struct session_request *sr;

    int fd_file;
    long bytes_offset, bytes_to_send;

    struct mk_list _head;
};

void cache_reqs_init() {

    struct mk_list *reqs = malloc(sizeof(struct mk_list));
    mk_list_init(reqs);
    pthread_setspecific(cache_reqs, reqs);
}

struct cache_req_t * cache_reqs_new() {
    struct cache_req_t *req = malloc(sizeof(struct cache_req_t));
    struct mk_list *reqs = pthread_getspecific(cache_reqs);
    mk_list_add(&req->_head, reqs);
    mk_info("creating a request!");

    return req;
}

struct cache_req_t * cache_reqs_get(int socket, struct session_request *sr) {
  struct mk_list *reqs = pthread_getspecific(cache_reqs);
  struct mk_list *curr;

  mk_list_foreach(curr, reqs) {
    struct cache_req_t *req = mk_list_entry(curr, struct cache_req_t, _head);
    if (req->socket == socket && req->sr == sr) {
      return req;
    }
  }

  return NULL;
}

void cache_reqs_del(int socket) {
  struct mk_list *reqs = pthread_getspecific(cache_reqs);
  struct mk_list *curr, *next;

  mk_list_foreach_safe(curr, next, reqs) {
    struct cache_req_t *req = mk_list_entry(curr, struct cache_req_t, _head);
    if (req->socket == socket) {
      mk_info("removing a requet!");
      mk_list_del(&req->_head);
      free(req);
    }
  }
}


const mk_pointer mk_default_mime = mk_pointer_init("text/plain\r\n");

int _mkp_init(struct plugin_api **api, char *confdir) {
    (void) confdir;

    mk_api = *api;

    return 0;
}

void _mkp_exit() {}

int _mkp_core_prctx(struct server_config *config) {
    (void) config;

    mk_info("cache: a new process ctx!!");

    pthread_key_create(&cache_reqs, NULL);

    return 0;
}
void _mkp_core_thctx() {
    mk_info("cache: a new thread ctx!!");

    cache_reqs_init();
}

int http_send_file(struct cache_req_t *req)
{

    long nbytes = mk_api->socket_send_file(req->socket, req->fd_file,
                                 &req->bytes_offset, req->bytes_to_send);

    if (nbytes > 0) {
        req->bytes_to_send -= nbytes;
    }

    if (nbytes < 0) {
      perror("cannto send file!");

      return -1;
    }

    return req->bytes_to_send;
}

int _mkp_stage_30(struct plugin *plugin, struct client_session *cs,
                  struct session_request *sr)
{
    (void) plugin;

    if (sr->file_info.size < 0 || sr->file_info.is_file == MK_FALSE || sr->file_info.read_access == MK_FALSE || sr->method != HTTP_METHOD_GET) {
        mk_info("not a file, passing on the file :)");
        return MK_PLUGIN_RET_NOT_ME;
    }

    // mk_info("cache plugin taking over the request :)");

    struct cache_req_t *req = cache_reqs_new();
    req->sr = sr;
    req->socket = cs->socket;


    sr->headers.last_modified = sr->file_info.last_modification;

    mk_api->header_set_http_status(sr, MK_HTTP_OK);

    sr->headers.content_length = sr->file_info.size;
    sr->headers.real_length = sr->file_info.size;

    // sr->fd_file = open(sr->real_path.data, sr->file_info.flags_read_only);
    req->fd_file = open(sr->real_path.data, sr->file_info.flags_read_only);

    if (req->fd_file == -1) {
        return MK_PLUGIN_RET_NOT_ME;
    }

    // sr->bytes_to_send = sr->file_info.size;
    sr->bytes_to_send = req->bytes_offset = 0;
    req->bytes_to_send = sr->file_info.size;

    sr->headers.content_type = mk_default_mime;

    mk_api->header_send(cs->socket, cs, sr);

    return MK_PLUGIN_RET_CONTINUE;
}


int _mkp_stage_40(struct client_session *cs, struct session_request *sr) {

  struct cache_req_t *req = cache_reqs_get(cs->socket, sr);
  if (req == NULL) {
    mk_info("not our request from stage 40!");
    return MK_PLUGIN_RET_NOT_ME;
  }

  if (http_send_file(req) <= 0) {
      mk_info("closing file fd and ending stage 40");
      close(req->fd_file);
      return MK_PLUGIN_RET_END;
  }
  else {
      mk_info("send some data, iterating in stage 40 again!");
      return MK_PLUGIN_RET_CONTINUE;
  }
}

int _mkp_stage_50(int socket) {
    cache_reqs_del(socket);

    return 0;
}
