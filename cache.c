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
pthread_key_t pool_reqs;

struct table_t *inode_table;
pthread_rwlock_t table_rwlock = PTHREAD_RWLOCK_INITIALIZER;

int devnull;

int createpipe(int *fds) {
  int size = 128 * 1024;
  if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0) {
      perror("cannot create a pipe!");
      return 0;
  }

  // optimisation on linux, increase pipe size
  fcntl(fds[1], F_SETPIPE_SZ, size);

  return size;

}

void closepipe(int *fds) {
  close(fds[0]);
  close(fds[1]);
}

void flushpipe(int *fds, int size) {
    printf("flushing what ever is left of pipe: %d\n", size);
    int res = splice(fds[0], NULL, devnull, NULL, size, SPLICE_F_MOVE);
    if (res < 0) {
        perror("cannot flush pipe!!!\n");
    }
    mk_bug(res != size);
}
void cache_pipe_init() {
  devnull = open("/dev/null", O_WRONLY);
  int *fds = calloc(sizeof(int), 2);
  createpipe(fds);
  pthread_setspecific(cache_pipe, fds);
}

struct pipe_buf_t {
  int pipe[2];
  int filled;     // amount of data filled in pipe
  int cap;        // tatal buffer size of pipe (should block afterwards)

  struct mk_list _head;
};

void pipe_buf_free(struct pipe_buf_t *buf) {
    closepipe(buf->pipe);
}


struct cache_file_t {
  // open fd of the file
  int fd;

  void *mmap;
  int mmap_len;

  struct stat st;

  // list of pipe_buf_t
  struct mk_list cache;

  // the offset after which the file data is stored
  // (for adding aux data like headers)
  long cache_offset;

};

struct cache_req_t {
    int socket;
    struct session_request *sr;

    struct cache_file_t *file;

    // pending file data to be send
    struct pipe_buf_t *curr;
    long bytes_offset, bytes_to_send;


    // pipe buffer for the request
    struct pipe_buf_t buf;

    // if the request is still alive (for reuse of cache_req_t)
    int active;
    struct mk_list _head;
};

void cache_reqs_init() {

    struct mk_list *reqs = malloc(sizeof(struct mk_list));
    mk_list_init(reqs);
    pthread_setspecific(cache_reqs, reqs);
}

void pool_reqs_init() {
    struct mk_list *pool = malloc(sizeof(struct mk_list));
    mk_list_init(pool);
    pthread_setspecific(pool_reqs, pool);
}


struct cache_req_t * cache_reqs_new() {
    struct mk_list *pool = pthread_getspecific(pool_reqs);
    struct cache_req_t *req;

    if (mk_list_is_empty(pool) == -1) {
        // printf("reusing an exhisting request!\n");
        req = mk_list_entry_first(pool, struct cache_req_t, _head);
        if (req->buf.filled) {
            flushpipe(req->buf.pipe, req->buf.filled);
        }

        mk_list_del(&req->_head);

    }
    else {
        req = malloc(sizeof(struct cache_req_t));
        req->buf.cap = createpipe(req->buf.pipe);
        req->buf.filled = 0;
    }


    req->socket = -1;
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

      // reuse the request as pipe creation can be expensive
      mk_list_add(&req->_head, pthread_getspecific(pool_reqs));

      // pipe_buf_free(&req->buf);
      // free(req);
      return;
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
    pthread_key_create(&pool_reqs, NULL);
    pthread_key_create(&cache_pipe, NULL);

    inode_table = table_alloc();

    return 0;
}
void _mkp_core_thctx() {
    mk_info("cache: a new thread ctx!!");

    cache_reqs_init();
    cache_pipe_init();
    pool_reqs_init();
}

int http_send_file(struct cache_req_t *req)
{
    long nbytes = mk_api->socket_send_file(req->socket, req->file->fd,
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


int http_send_mmap_zcpy(struct cache_req_t *req) {
    struct cache_file_t *file = req->file;
    long pbytes;


    // loop until the current file pointer is a the end
    // or tmp buffer still has data
    while (req->bytes_to_send > 0 && (req->curr || req->buf.filled)) {
        // until range requests are not supported, this is a bug
        mk_bug(req->bytes_offset + req->bytes_to_send != file->st.st_size);

        struct pipe_buf_t *curr = req->curr;
        // fill in buf pipe with req data
        if (curr) {
            // offset of the file till the end of filled cache
            long off = req->bytes_offset + req->buf.filled +
              req->curr->filled;

            // amount of data that can be filled from file to cache
            long len = curr->cap - curr->filled;
            if (off + len > file->st.st_size) {
                //  this should be the last portion of file
                len = file->st.st_size - off;
                /*
                if (len > 0)
                    printf("hopefully the last portion of file "
                        "(namely %ld bytes) can be filled!\n",
                        len);
                */
            }

            mk_bug(off > file->st.st_size);
            mk_bug(len < 0);

            if (len > 0) {
                struct iovec tmp = {
                  .iov_base = file->mmap + off,
                  .iov_len = len
                };

                pbytes = vmsplice(curr->pipe[1], &tmp, 1,
                    SPLICE_F_NONBLOCK | SPLICE_F_GIFT);

                if (pbytes < 0) {
                    if (errno == EAGAIN) {
                        // opened file is blocking, carry on sending data
                        // to the socket
                        goto SEND_BUFFER;
                    }

                    perror(
                      "cannot vmsplice data form file cache to the "
                      "pipe buffer!\n");

                    printf("Tried to write from %ld (out of %ld) till "
                        "%ld\n", off, file->st.st_size, off + len);
                    return -1;
                }
                // printf("filled %ld into file cache!\n", pbytes);

                curr->filled += pbytes;
                len -= pbytes;
            }

            // if buffer is empty and cache is all set, fill it with the cache
            if (len == 0 && !req->buf.filled) {
                // fill curr into buf
                pbytes = tee(curr->pipe[0], req->buf.pipe[1],
                    curr->filled, SPLICE_F_NONBLOCK);

                if (pbytes < 0) {
                    if (errno == EAGAIN) {
                        mk_info("Pretty strange, blocking in tee?!");
                        goto SEND_BUFFER;
                    }
                    else {
                        perror("Cannot tee into tmp buf!!\n");
                        return -1;
                    }
                }

                // printf("filled %ld from file cache to req buffer!\n",
                //    pbytes);


                req->buf.filled += pbytes;



                if (&file->cache == curr->_head.next) {
                    // reached the start so finished with file buffers

                    // printf("cache finished, only buffer needs to be "
                    //    "flushed (%d bytes)!\n", req->buf.filled);
                    req->curr = curr = NULL;
                }
                else {
                  req->curr = curr = mk_list_entry_next(&curr->_head,
                      struct pipe_buf_t, _head, &file->cache);
                }

            }
        }

SEND_BUFFER:
        if (!req->buf.filled) {
           // nothing to do, just get out of the loop
           // mk_info("surprisingly no data to send over to the socket!\n");
           break;
        }

        // finally splicing data to the socket
        pbytes = splice(req->buf.pipe[0], NULL, req->socket, NULL,
            req->buf.filled, SPLICE_F_NONBLOCK | SPLICE_F_MOVE);
        if (pbytes < 0) {
            if (errno == EAGAIN) {
                // blocking socket, lets try in the next cycle
                // printf("socket blocking, trying again in the next cycle!\n");
                break;
            }

            perror("Cannot splice data to the socket!\n");
            return -1;
        }

        req->buf.filled -= pbytes;
        req->bytes_to_send -= pbytes;
        req->bytes_offset += pbytes;

        // printf("writing %ld bytes of data over the socket!\n", pbytes);

        mk_bug(req->bytes_to_send < 0);

        if (req->buf.filled) {
            //mk_info("not all data was sent, i can tell that its gonna "
            //    "block next time ;)");
        }
    }

    // printf("Ending cycle, bytes left: %ld\n\n", req->bytes_to_send);
    return req->bytes_to_send;
}

int serve_req(struct cache_req_t *req) {
    if (req->file->mmap == (void *) -1) {
        mk_info("mmap invalid, sending standard file");
        return http_send_file(req);
    }

    return http_send_mmap_zcpy(req);
}
int _mkp_event_write(int fd) {
    struct cache_req_t *req = cache_reqs_get(fd);
    if (!req) {
        //mk_info("write event, but not of our request");
        return MK_PLUGIN_RET_EVENT_NEXT;
    }
    //printf("handling write event for our request!\n");
    if (req->bytes_to_send <= 0) {
        mk_info("no data to send, returning event_write!");
        return MK_PLUGIN_RET_EVENT_CLOSE;
    }
    int ret = serve_req(req);

    if (ret <= 0) {

        cache_reqs_del(fd);
        mk_api->http_request_end(fd);
        //printf("dont with the request, ending it!\n");

        return MK_PLUGIN_RET_EVENT_OWNED;
    }
    else {
        //printf("send some data, calling the stages again!\n");

        return MK_PLUGIN_RET_EVENT_CONTINUE;
    }
}

int _mkp_stage_30(struct plugin *plugin, struct client_session *cs,
                  struct session_request *sr)
{
    (void) plugin;
    struct stat st;

    //mk_info("running stage 30");

    if (sr->file_info.size < 0 ||
      sr->file_info.is_file == MK_FALSE ||
      sr->file_info.read_access == MK_FALSE ||
      sr->method != HTTP_METHOD_GET) {

        mk_info("not a file, passing on the file :)");
        return MK_PLUGIN_RET_NOT_ME;
    }


    struct cache_req_t *req = cache_reqs_get(cs->socket);
    if (req) {
      //printf("got back an old request, continuing stage 30!\n");
      return MK_PLUGIN_RET_CONTINUE;
    }
    if (!req) {
      req = cache_reqs_new();
    }

    req->sr = sr;
    req->socket = cs->socket;

    //sr->headers.last_modified = sr->file_info.last_modification;

    mk_api->header_set_http_status(sr, MK_HTTP_OK);

    sr->headers.content_length = sr->file_info.size;
    sr->headers.real_length = sr->file_info.size;

    stat(sr->real_path.data, &st);

    // sr->bytes_to_send = sr->file_info.size;
    req->bytes_offset = 0;
    req->bytes_to_send = st.st_size;

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
            printf("creating a new file cache with path %s and size %ld\n",
                sr->real_path.data, st.st_size);
            file = malloc(sizeof(struct cache_file_t));
            mk_list_init(&file->cache);

            file->fd = open(sr->real_path.data,
                sr->file_info.flags_read_only | O_NONBLOCK);
            file->st = st;

            file->mmap_len =
                ceil((double)sr->file_info.size / MMAP_SIZE) * MMAP_SIZE;

            file->mmap = mmap(NULL, file->mmap_len,
                PROT_READ, MAP_PRIVATE, file->fd, 0);

            if (file->mmap == (void* ) -1) {
                perror("cannot open file!");
                exit(0);
                return MK_PLUGIN_RET_NOT_ME;
            }

            // TODO: append http headers to file buffer
            file->cache_offset = 0;

            // file file buffer with empty pipes for now
            long len = file->st.st_size;
            struct pipe_buf_t *tmp;
            while (len > 0) {
                tmp  = malloc(sizeof(struct pipe_buf_t));
                tmp->filled = 0;
                tmp->cap = createpipe(tmp->pipe);
                mk_list_add(&tmp->_head, &file->cache);

                len -= tmp->cap;
            }

            table_add(inode_table, st.st_ino, file);
        }
        pthread_rwlock_unlock(&table_rwlock);
    }
    else {
        //printf("got file with inode: %ld\n", file->st.st_ino);
    }

    // sr->fd_file = open(sr->real_path.data, sr->file_info.flags_read_only);
    req->file = file;
    req->curr = mk_list_entry_first(&file->cache,
        struct pipe_buf_t, _head);



    sr->headers.content_type = mk_default_mime;

    mk_api->header_send(cs->socket, cs, sr);


    if (serve_req(req) <= 0) {
        mk_api->socket_cork_flag(req->socket, TCP_CORK_OFF);
        //printf("ending request early!\n");

        cache_reqs_del(req->socket);
        return MK_PLUGIN_RET_END;
    }

    mk_api->socket_cork_flag(req->socket, TCP_CORK_OFF);

    return MK_PLUGIN_RET_CONTINUE;
}


int _mkp_event_error(int socket)
{
    mk_info("got an error with a socket!");
    cache_reqs_del(socket);
    return MK_PLUGIN_RET_EVENT_NEXT;
}
int _mkp_event_timeout(int socket)
{
    mk_info("got an error with a socket!");
    cache_reqs_del(socket);
    return MK_PLUGIN_RET_EVENT_NEXT;
}
