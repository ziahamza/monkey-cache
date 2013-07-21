/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netdb.h>

#define _GNU_SOURCE
#define __USE_GNU

#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#define MMAP_SIZE sysconf(_SC_PAGE_SIZE)

#include <sys/uio.h>

#include <math.h>

#include "MKPlugin.h"

#include "ht.h"
#include "utils.h"

MONKEY_PLUGIN("cache",         /* shortname */
              "Monkey Cache", /* name */
              VERSION,        /* version */
              MK_PLUGIN_CORE_PRCTX | MK_PLUGIN_CORE_THCTX |  MK_PLUGIN_STAGE_30); /* hooks */

pthread_key_t cache_pipe;
pthread_key_t cache_reqs;

struct table_t *inode_table;
pthread_rwlock_t table_rwlock = PTHREAD_RWLOCK_INITIALIZER;

int devnull;

int createpipe(int *fds) {
  int size = 128 * 1024;
  if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0) {
      perror("cannot create a thread specific pipe!");
      return 0;
  }

  // optimisation on linux, increase pipe size
  fcntl(fds[1], F_SETPIPE_SZ, 512 * 1024);

  return size;

}

void closepipe(int *fds) {
  close(fds[0]);
  close(fds[1]);
}

void cache_pipe_init() {
  devnull = open("/dev/null", O_WRONLY);
  int *fds = calloc(sizeof(int), 2);
  createpipe(fds);
  pthread_setspecific(cache_pipe, fds);
}

struct cache_file_t {
  int mmap_len;
  void *mmap;

  int pipe_buf[2];
  int pipe_filled;
  int pipe_cap;

  struct stat st;
};

struct cache_req_t {
    int socket;
    struct session_request *sr;

    int fd_file;
    struct cache_file_t *file;
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
    //mk_info("creating a request!");

    return req;
}

struct cache_req_t * cache_reqs_get(int socket) {
  struct mk_list *reqs = pthread_getspecific(cache_reqs);
  struct mk_list *curr;

  mk_list_foreach(curr, reqs) {
    struct cache_req_t *req = mk_list_entry(curr, struct cache_req_t, _head);
    if (req->socket == socket) {
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
      //mk_info("removing a requet!");
      mk_list_del(&req->_head);
      free(req);
    };
  }


}


const mk_pointer mk_default_mime = mk_pointer_init("text/plain\r\n");

int _mkp_init(struct plugin_api **api, char *confdir) {
    (void) confdir;

    mk_api = *api;

    return 0;
}

void _mkp_exit() {
    table_free(inode_table);
    // TODO: free cache_reqs
}

int _mkp_core_prctx(struct server_config *config) {
    (void) config;

    mk_info("cache: a new process ctx!!");

    pthread_key_create(&cache_reqs, NULL);
    pthread_key_create(&cache_pipe, NULL);

    inode_table = table_alloc();

    return 0;
}
void _mkp_core_thctx() {
    mk_info("cache: a new thread ctx!!");

    cache_reqs_init();
    cache_pipe_init();
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

void flush_pipe(int wfd, int size) {
    //printf("flushing what ever is left of pipe: %d\n", size);
    splice(wfd, NULL, devnull, NULL, size, SPLICE_F_MOVE);
}

int http_send_mmap_zcpy(struct cache_req_t *req) {
    int *fds;

    fds = pthread_getspecific(cache_pipe);

    struct cache_file_t *file = req->file;
    long nbytes = 1, pbytes = 1, cnt = 0;

    struct iovec vec = {
      .iov_base = file->mmap + req->bytes_offset,
      .iov_len = req->bytes_to_send
    };

    if (!file->pipe_filled) {
        pbytes = vmsplice(file->pipe_buf[1], &vec, 1,
            SPLICE_F_NONBLOCK | SPLICE_F_GIFT);

        //printf("filled initial into file buffer %ld\n", pbytes);

      if (pbytes < 0) {
          perror("cannot read to pipe buffer!!");
          printf(
              "bytes offset: %ld bytes to send: %ld mmap address: %p\n",
              req->bytes_offset, req->bytes_to_send, vec.iov_base);
          return -1;
        }
        file->pipe_filled = pbytes;
    }

    pbytes = tee(file->pipe_buf[0], fds[1], file->pipe_filled,
        SPLICE_F_NONBLOCK);
    if (pbytes < 0) {
        perror("cannot tee from file cache!");
        return -1;
    }
    //printf("written data from inital file buffer %ld\n", pbytes);

    while (1) {
        nbytes = splice(fds[0], NULL, req->socket, NULL, pbytes,
            SPLICE_F_NONBLOCK | SPLICE_F_MOVE);
        //printf("written data from pipe buffer %ld\n", nbytes);
        if (nbytes < 0) {
          flush_pipe(fds[0], pbytes);
          if (errno == EAGAIN) {
            //printf("socket could block, returning!");
            break;
          }
          perror("cannot splice bytes into the socket");
          return -1;
        }

        cnt++;

        req->bytes_to_send -= nbytes;
        req->bytes_offset += nbytes;

        if (pbytes != nbytes) {
          flush_pipe(fds[0], pbytes - nbytes);
          // next iteration will probably block, return early
          break;
        }

        if (!req->bytes_to_send) break;

        vec.iov_base = file->mmap + req->bytes_offset;
        vec.iov_len = req->bytes_to_send;

        pbytes = vmsplice(fds[1], &vec, 1,
            SPLICE_F_NONBLOCK | SPLICE_F_GIFT);

        //printf("pumping data into pipe! %ld\n", pbytes);

        if (pbytes < 0) {
            perror("cannot read to pipe buffer!!");
            printf(
                "bytes offset: %ld bytes to send: %ld mmap address: %p\n",
                req->bytes_offset, req->bytes_to_send, vec.iov_base);
            return -1;
        }

    }

    //printf("total splice iterations %ld\n", cnt);
    //printf("bytes left: %ld\n", req->bytes_to_send);

    return req->bytes_to_send;
}
int http_send_mmap(struct cache_req_t *req) {

    long nbytes = 1;

    while (nbytes > 0 && req->bytes_to_send) {
      nbytes = mk_api->socket_send(req->socket, req->file->mmap + req->bytes_offset, req->bytes_to_send);


      if (nbytes < 0) {
        if (errno == EAGAIN) {
          break;
        }

        perror("cannot write to socket!");
        return -1;
      }

      req->bytes_to_send -= nbytes;
      req->bytes_offset += nbytes;

    }

    return req->bytes_to_send;
}

int _mkp_event_write(int fd) {
    struct cache_req_t *req = cache_reqs_get(fd);
    if (!req) {
        //mk_info("write event, but not of our request");
        return MK_PLUGIN_RET_EVENT_NEXT;
    }
    if (req->bytes_to_send <= 0) {
        mk_info("no data to send, returning event_write!");
        return MK_PLUGIN_RET_EVENT_CLOSE;
    }
    int ret = 0;
    if (req->file->mmap != NULL || req->file->mmap != (void *) -1) {
        //mk_info("sending using mmap :)");
        ret = http_send_mmap_zcpy(req);
    }
    else {
        mk_info("mmap invalid, sending standard file");
        ret = http_send_file(req);
    }

    if (ret <= 0) {

        //mk_api->http_request_end(fd);

        return MK_PLUGIN_RET_EVENT_CLOSE;
    }
    else {
        //printf("send some data, iterating event write again!\n");

        return MK_PLUGIN_RET_EVENT_OWNED;
    }
}

int _mkp_stage_30(struct plugin *plugin, struct client_session *cs,
                  struct session_request *sr)
{
    (void) plugin;
    struct stat st;

    //mk_info("running stage 30");

    if (sr->file_info.size < 0 || sr->file_info.is_file == MK_FALSE || sr->file_info.read_access == MK_FALSE || sr->method != HTTP_METHOD_GET) {
        //mk_info("not a file, passing on the file :)");
        return MK_PLUGIN_RET_NOT_ME;
    }

    // mk_info("cache plugin taking over the request :)");

    struct cache_req_t *req = cache_reqs_new();
    req->sr = sr;
    req->socket = cs->socket;


    //sr->headers.last_modified = sr->file_info.last_modification;

    mk_api->header_set_http_status(sr, MK_HTTP_OK);

    sr->headers.content_length = sr->file_info.size;
    sr->headers.real_length = sr->file_info.size;

    // sr->bytes_to_send = sr->file_info.size;
    req->bytes_offset = 0;
    req->bytes_to_send = sr->file_info.size;

    stat(sr->real_path.data, &st);
    //printf("\n\ntrying to get cache for path %s with inode %ld\n",
    //    sr->real_path.data, st.st_ino);

    //printf("headers:\n%s\n", sr->uri_processed.data);
    int cnt = 0;
    while (1) {
      int rc = pthread_rwlock_tryrdlock(&table_rwlock);
      if (rc == EBUSY) {
        if (cnt > 10) {
          printf("tried locking too many times, giving up!");
          return MK_PLUGIN_RET_NOT_ME;
        }

        printf("could not get lock, trying again!\n");
        cnt++;
      }
      else break;
    }
    struct cache_file_t *file = table_get(inode_table, st.st_ino);
    pthread_rwlock_unlock(&table_rwlock);
    if (!file) {
        pthread_rwlock_wrlock(&table_rwlock);
        // another check in case its been already added
        file = table_get(inode_table, st.st_ino);

        if (!file) {
            printf("creating a new file cache with path %s\n",
                sr->real_path.data);
            file = malloc(sizeof(struct cache_file_t));

            int fd = open(sr->real_path.data,
                sr->file_info.flags_read_only | O_NONBLOCK);
            file->st = st;

            file->mmap_len = ceil(
                (double)sr->file_info.size / MMAP_SIZE) * MMAP_SIZE;

            file->mmap = mmap(NULL, file->mmap_len,
                PROT_READ, MAP_PRIVATE, fd, 0);
            if (file->mmap == (void* ) -1) {
                perror("cannot open file!");
                exit(0);
                return MK_PLUGIN_RET_NOT_ME;
            }

            file->pipe_cap = createpipe(file->pipe_buf);
            file->pipe_filled = 0;
            close(fd);

            table_add(inode_table, st.st_ino, file);
        }
        pthread_rwlock_unlock(&table_rwlock);
    }
    else {
        //printf("got file with inode: %ld\n", file->st.st_ino);
    }

    // sr->fd_file = open(sr->real_path.data, sr->file_info.flags_read_only);
    req->fd_file = -1;
    req->file = file;



    sr->headers.content_type = mk_default_mime;

    mk_api->header_send(cs->socket, cs, sr);


    if (_mkp_event_write(cs->socket) == MK_PLUGIN_RET_EVENT_CLOSE) {
      mk_api->socket_cork_flag(req->socket, TCP_CORK_OFF);
      return MK_PLUGIN_RET_END;
    }

    mk_api->socket_cork_flag(req->socket, TCP_CORK_OFF);

    return MK_PLUGIN_RET_CONTINUE;
}


void cleanup(int fd) {
    struct cache_req_t *req = cache_reqs_get(fd);
    if (!req) {
      return;
    }

    if (req->fd_file >= 0)
        close(req->fd_file);

    cache_reqs_del(fd);


}
int _mkp_event_close(int socket)
{
  //mk_info("closing a socket");

  cleanup(socket);
  return MK_PLUGIN_RET_EVENT_NEXT;
}

int _mkp_event_error(int socket)
{
  mk_info("got an error with a socket!");
  cleanup(socket);
	return MK_PLUGIN_RET_EVENT_NEXT;
}
int _mkp_event_timeout(int socket)
{
  mk_info("got an error with a socket!");
  cleanup(socket);
	return MK_PLUGIN_RET_EVENT_NEXT;
}
