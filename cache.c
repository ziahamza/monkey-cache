/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

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
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <sys/mman.h>
#define MMAP_SIZE sysconf(_SC_PAGE_SIZE)

#include <sys/uio.h>

#include <math.h>

#include "MKPlugin.h"
#include "mk_mimetype.h"

#include "ht.h"
#include "utils.h"

#include "cJSON.h"

#define API_PREFIX "/cache"
#define API_PREFIX_LEN 6

#define MAX_PATH_LEN 1024
#define MAX_URI_LEN 512

// size of a single pipe
#define PIPE_SIZE 512 * 1024

MONKEY_PLUGIN("cache",         /* shortname */
              "Monkey Cache", /* name */
              VERSION,        /* version */
              MK_PLUGIN_CORE_PRCTX | MK_PLUGIN_CORE_THCTX |  MK_PLUGIN_STAGE_30); /* hooks */

pthread_key_t cache_reqs;
pthread_key_t pool_reqs;
pthread_key_t pipe_buf_pool;

struct table_t *file_table;
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

int devnull;
// total memory reserved by the plugin in form of pipes
// TODO: make it either atomic or protected by a lock
int pipe_totalmem = 0;

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

    pipe_totalmem += PIPE_SIZE;

    return PIPE_SIZE;
}

void closepipe(int *fds) {
    close(fds[0]);
    close(fds[1]);

    pipe_totalmem -= PIPE_SIZE;
}

void flushpipe(int *fds, int size) {
    printf("flushing what ever is left of pipe: %d\n", size);
    int res = splice(fds[0], NULL, devnull, NULL, size, SPLICE_F_MOVE);
    if (res < 0) {
        perror("cannot flush pipe!!!\n");
    }
    mk_bug(res != size);
}

struct pipe_buf_t {
    int pipe[2];
    int filled;     // amount of data filled in pipe
    int cap;        // tatal buffer size of pipe (should block afterwards)

    pthread_mutex_t write_mutex; // write access to the buffer

    struct mk_list _head;
};


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
};

struct cache_req_t {
    int socket;

    struct cache_file_t *file;

    // pending file data to be send
    struct pipe_buf_t *curr;
    long bytes_offset, bytes_to_send;


    // pipe buffer for the request
    struct pipe_buf_t *buf;

    struct mk_list _head;
};

void cache_reqs_init() {

    struct mk_list *reqs = mk_api->mem_alloc(sizeof(struct mk_list));
    mk_list_init(reqs);
    pthread_setspecific(cache_reqs, reqs);
}

void pool_reqs_init() {
    struct mk_list *pool = mk_api->mem_alloc(sizeof(struct mk_list));
    mk_list_init(pool);
    pthread_setspecific(pool_reqs, pool);
}

void pipe_buf_pool_init() {
    struct mk_list *pool = mk_api->mem_alloc(sizeof(struct mk_list));
    mk_list_init(pool);
    pthread_setspecific(pipe_buf_pool, pool);
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
        buf = mk_api->mem_alloc(sizeof(struct pipe_buf_t));

        buf->filled = 0;
        buf->cap = createpipe(buf->pipe);

        pthread_mutex_init(&buf->write_mutex, NULL);
    }

    mk_bug(buf->filled != 0);
    return buf;
}
void pipe_buf_free(struct pipe_buf_t *buf) {
    mk_list_add(&buf->_head, pthread_getspecific(pipe_buf_pool));
}

struct cache_req_t *cache_req_new() {
  struct mk_list *pool = pthread_getspecific(pool_reqs);
  struct cache_req_t *req;

    if (mk_list_is_empty(pool) == -1) {
        // printf("reusing an exhisting request!\n");
        req = mk_list_entry_first(pool, struct cache_req_t, _head);

        mk_list_del(&req->_head);

    }
    else {
        req = mk_api->mem_alloc(sizeof(struct cache_req_t));
    }

    req->buf = pipe_buf_new();


    req->socket = -1;
    struct mk_list *reqs = pthread_getspecific(cache_reqs);
    mk_list_add(&req->_head, reqs);
    //mk_info("creating a request!");

    mk_bug(req->buf->filled != 0);
    return req;
}

struct cache_req_t *cache_reqs_get(int socket) {
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

void cache_req_del(struct cache_req_t *req) {
    pipe_buf_free(req->buf);

    req->buf = NULL;
    mk_list_del(&req->_head);


    // reuse the request as pipe creation can be expensive
    mk_list_add(&req->_head, pthread_getspecific(pool_reqs));

}

void cache_reqs_del(int socket) {
    struct mk_list *reqs = pthread_getspecific(cache_reqs);
    struct mk_list *curr, *next;

    mk_list_foreach_safe(curr, next, reqs) {
        struct cache_req_t *req = mk_list_entry(curr, struct cache_req_t, _head);
        if (req->socket == socket) {
            cache_req_del(req);
            return;
        };
    }

}

const mk_pointer mk_default_mime = mk_pointer_init("text/plain\r\n");
char conf_dir[MAX_PATH_LEN];
int conf_dir_len;

#define MIME_MAX_LEN 128
#define MIME_LOOKUP_MAX 10
struct mime_map_t {
    // file extention
    char ext[MIME_MAX_LEN];

    // actual mime
    char mime[MIME_MAX_LEN];
};

struct mime_map_t mime_map[MIME_LOOKUP_MAX];

int _mkp_init(struct plugin_api **api, char *confdir) {

    char config_path[MAX_PATH_LEN];

    mk_api = *api;
    strncpy(conf_dir, confdir, MAX_PATH_LEN);
    conf_dir_len = strlen(confdir);


    snprintf(config_path, MAX_PATH_LEN, "%scache.conf", confdir);
    struct mk_config *cnf = mk_api->config_create(config_path);

    struct mk_config_section *section = mk_api->config_section_get(cnf, "MIMETYPES");
    struct mk_list *mime_head;
    struct mk_config_entry *entry;

    int i = 0;
    mk_list_foreach(mime_head, &section->entries) {
        entry = mk_list_entry(mime_head, struct mk_config_entry, _head);

        if (i < MIME_LOOKUP_MAX) {
            strncpy(mime_map[i].ext, entry->key, MIME_MAX_LEN);
            snprintf(mime_map[i].mime, MIME_MAX_LEN, "%s\r\n", entry->val);
        }
        else {
            break;
        }
        i++;
    }

    mk_api->config_free(cnf);

    return 0;
}

void _mkp_exit() {
    table_free(file_table);
    // TODO: free cache_reqs
}

int _mkp_core_prctx(struct server_config *config) {
    (void) config;

    mk_info("cache: a new process ctx!!");
    devnull = open("/dev/null", O_WRONLY);

    pthread_key_create(&cache_reqs, NULL);
    pthread_key_create(&pool_reqs, NULL);

    file_table = table_alloc();

    return 0;
}
void _mkp_core_thctx() {
    mk_info("cache: a new thread ctx!!");

    cache_reqs_init();
    pool_reqs_init();
    pipe_buf_pool_init();
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

int mem_to_pipe(void *mem, struct pipe_buf_t *dest, int off, int len) {
    int cnt = 0;
    pthread_mutex_lock(&dest->write_mutex);
    if (dest->cap - dest->filled  < len) {
        mk_bug("destination pipe doesnt have enough space!");
    }

    struct iovec tmp = {
        .iov_base = mem + off,
        .iov_len = len
    };

    mk_bug(len < 0);

    while (cnt < len) {

        int ret = vmsplice(dest->pipe[1], &tmp, 1,
            SPLICE_F_NONBLOCK | SPLICE_F_GIFT);

        if (ret < 0) {
            if (errno == EAGAIN) {
                // opened file is blocking, exit out
                return cnt;
            }

            perror(
              "cannot vmsplice data form file cache to the "
              "pipe buffer!\n");

            return ret;
        }
        dest->filled += ret;
        cnt += ret;
    }

    pthread_mutex_unlock(&dest->write_mutex);
    return cnt;
}

// fill the current file cache pointer of a request
int fill_req_curr(struct cache_req_t *req) {
    struct pipe_buf_t *curr = req->curr;
    struct cache_file_t *file = req->file;

    // offset of the file before the start of filled cache
    long off = req->bytes_offset + req->buf->filled;

    // total amount of data that can be filled from file to cache
    long len = curr->cap;

    if (off + len > file->size) {
        //  this should be the last portion of file
        len = file->size - off;
        /*
        if (len > 0)
            printf("hopefully the last portion of file "
                "(namely %ld bytes) can be filled!\n",
                len);
        */
    }

    mk_bug(off > file->size);
    mk_bug(len < 0);

    if (curr->filled == len) return 0;

    if (curr->filled < len) {

        if (mem_to_pipe(file->buf.data, curr, off + curr->filled, len - curr->filled) < 0) {

            printf("Tried to write from %ld (out of %ld) till "
                "%ld\n", off, file->size, off + len);
        }
    }

    // mk_bug(curr->filled > len);

    return len - curr->filled;
}

int http_send_mmap_zcpy(struct cache_req_t *req) {
    struct cache_file_t *file = req->file;
    long pbytes;

    // loop until the current file pointer is a the end or tmp buffer
    // still has data
    while (req->bytes_to_send > 0 && (req->curr || req->buf->filled)) {
        // until range requests are not supported, this is a bug
        fflush(stdout);
        mk_bug(req->bytes_offset + req->bytes_to_send != file->size);

        // fill in buf pipe with req data
        // if buffer is empty and cache is all set, fill it with the cache
        if (req->curr && fill_req_curr(req) == 0 && !req->buf->filled) {
            // fill curr into buf
            pbytes = tee(req->curr->pipe[0], req->buf->pipe[1],
                req->curr->filled, SPLICE_F_NONBLOCK);

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


            req->buf->filled += pbytes;

            if (&file->cache == req->curr->_head.next) {
                // reached the start so finished with file buffers

                // printf("cache finished, only buffer needs to be "
                //    "flushed (%d bytes)!\n", req->buf->filled);
                req->curr =  NULL;
            }
            else {
                req->curr = mk_list_entry_next(&req->curr->_head,
                  struct pipe_buf_t, _head, &file->cache);
            }

        }

SEND_BUFFER:
        if (!req->buf->filled) {
            // nothing to do, just get out of the loop
            // mk_info("surprisingly no data to send over to the socket!\n");
            break;
        }

        // finally splicing data to the socket
        pbytes = splice(req->buf->pipe[0], NULL, req->socket, NULL,
            req->buf->filled, SPLICE_F_NONBLOCK | SPLICE_F_MOVE);
        if (pbytes < 0) {
            if (errno == EAGAIN) {
                // blocking socket, lets try in the next cycle
                // printf("socket blocking, trying again in the next cycle!\n");
                break;
            }

            perror("Cannot splice data to the socket!\n");
            return -1;
        }

        req->buf->filled -= pbytes;
        req->bytes_to_send -= pbytes;
        req->bytes_offset += pbytes;

        // printf("writing %ld bytes of data over the socket!\n", pbytes);

        mk_bug(req->bytes_to_send < 0);

        if (req->buf->filled) {
            //mk_info("not all data was sent, i can tell that its gonna "
            //    "block next time ;)");
        }
    }

    // printf("Ending cycle, bytes left: %ld\n\n", req->bytes_to_send);
    return req->bytes_to_send;
}

void serve_cache_headers(struct cache_req_t *req) {
    int ret = tee(req->file->cache_headers->pipe[0],
        req->buf->pipe[1], req->file->cache_headers->filled, SPLICE_F_NONBLOCK);

    if (ret < 0) {
        perror("cannot tee into the request buffer!!\n");
        mk_bug(1);
    }

    mk_bug(ret < req->file->header_len);

    req->buf->filled += ret;

    // HACK: make headers seem like file contents
    req->bytes_offset -= req->file->header_len;
    req->bytes_to_send += req->file->header_len;
}

int serve_req(struct cache_req_t *req) {
    if (req->file->buf.data == (void *) -1) {
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

mk_pointer get_mime(char *path) {
    const mk_pointer mime = mk_default_mime;
    int i, j;

    int len = strlen(path);
    for (j = len; j > 0; j--) {
        if (path[j - 1] != '.')
            continue;

        for (i = 0; i < MIME_LOOKUP_MAX; i++) {
            if (strcmp(path + j, mime_map[i].ext) == 0) {
                mk_pointer tmp = {
                    .data = mime_map[i].mime,
                    .len = strlen(mime_map[i].mime)
                };

                return tmp;
            }
        }
        break;
    }

    return mime;
}

int serve_stats(struct client_session *cs, struct session_request *sr)
{

    mk_api->header_set_http_status(sr, MK_HTTP_OK);

    cJSON *root, *mem, *files, *file;
    char *out;

    root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "memory", mem = cJSON_CreateObject());
    cJSON_AddItemToObject(root, "files", files = cJSON_CreateArray());
    cJSON_AddNumberToObject(mem,"pipe_size", PIPE_SIZE);
    cJSON_AddNumberToObject(mem,"pipe_total_memory", pipe_totalmem);

    int i;
    struct node_t *node;
    struct cache_file_t *f;
    for (i = 0; i < file_table->size; i++) {
        struct node_t *next;
        for (
          node = file_table->store[i];
          node != NULL;
          node = next
        ) {
            next = node->next;
            f = node->val;

            cJSON_AddItemToArray(files, file = cJSON_CreateObject());
            cJSON_AddStringToObject(file, "uri", f->uri);
            cJSON_AddNumberToObject(file, "size", f->size);

        }
    }

    out = cJSON_Print(root);


    sr->headers.content_length = strlen(out);
    sr->headers.content_type = get_mime(".json");

    mk_api->header_send(cs->socket, cs, sr);

    mk_api->socket_send(cs->socket, out, strlen(out));

    // printf("got a call for the api!\n");

    cJSON_Delete(root);
    free(out);
    return MK_PLUGIN_RET_END;

}

void cache_file_fill_headers(struct cache_file_t *file,
    struct client_session *cs, struct session_request *sr) {
    int ret = 0;
    pthread_mutex_lock(&file->cache_headers->write_mutex);
    if (!file->cache_headers->filled) {

        // HACK: change server request values to prevent monkey to
        // not add "connection: close" in any case as it messes up things
        // when served every request with same cached headers.
        int old_conn = sr->headers.connection = 0,
            old_keepalive = sr->keep_alive = MK_TRUE,
            old_close = sr->close_now = MK_FALSE,
            old_conlen = sr->connection.len;

        if (sr->connection.len == 0) sr->connection.len = 1;

        mk_api->header_send(file->cache_headers->pipe[1], cs, sr);

        if (ioctl(file->cache_headers->pipe[0], FIONREAD,
              &file->header_len) != 0) {
            perror("cannot find size of pipe buf!");
            mk_bug(1);
        }

        // restoring modified server request values
        sr->headers.connection = old_conn;
        sr->keep_alive = old_keepalive;
        sr->close_now = old_close;
        sr->connection.len = old_conlen;

        file->cache_headers->filled = file->header_len;
        // fill in the empty header pipe space with some initial file
        // data to send them in a single tee syscall, only in case
        // there is enough room which would be true for small files
        // which fit inside a pipe
        int leftover =
          file->cache_headers->cap - file->cache_headers->filled;

        struct pipe_buf_t *first_buf = mk_list_entry_first(&file->cache,
                struct pipe_buf_t, _head);

        if (leftover > first_buf->filled) {
           if (first_buf->filled) {
              ret = tee(first_buf->pipe[0],
                file->cache_headers->pipe[1], leftover,
                SPLICE_F_NONBLOCK);

              mk_bug(ret <= 0);

              file->cache_headers->filled += ret;
           }
        }
        else {
            // file too big to compltely fit in the header pipe
            // along with the rest of the headers
        }

        mk_bug(file->cache_headers->filled == 0);
    }
    pthread_mutex_unlock(&file->cache_headers->write_mutex);
}

struct cache_file_t *cache_file_new(const char *path, const char *uri) {
    struct cache_file_t *file;

    struct stat st;
    if (stat(path, &st) == -1 || S_ISDIR(st.st_mode)) {
        return NULL;
    }

    if (st.st_size <= 0)
        return NULL;

    pthread_mutex_lock(&table_mutex);

    // another check in case its been already added
    file = table_get(file_table, uri);

    if (!file) {
        printf("creating a new file cache with path %s and uri %s\n",
            path, uri);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd == -1) {
            perror("cannot open the file!");
            return NULL;
        }

        int mmap_len =
            ceil((double) st.st_size / MMAP_SIZE) * MMAP_SIZE;

        void *mmap_ptr = mmap(NULL, mmap_len,
            PROT_READ, MAP_PRIVATE, fd, 0);

        if (mmap_ptr == (void *) -1) {
            close(fd);
            perror("cannot create an mmap!");
            return NULL;
        }

        file = mk_api->mem_alloc(sizeof(struct cache_file_t));
        strncpy(file->uri, uri, MAX_URI_LEN);

        mk_list_init(&file->cache);

        file->size = st.st_size;

        file->buf.data = mmap_ptr;
        file->buf.len = mmap_len;


        file->cache_headers = pipe_buf_new();


        // file file buffer with empty pipes for now
        long len = file->size;
        struct pipe_buf_t *tmp;
        while (len > 0) {
            tmp = pipe_buf_new();

            mk_list_add(&tmp->_head, &file->cache);

            len -= tmp->cap;
        }

        table_add(file_table, file->uri, file);
    }
    pthread_mutex_unlock(&table_mutex);

    return file;
}
struct cache_file_t *cache_file_get(const char *uri) {
    return table_get(file_table, uri);
}

int _mkp_stage_30(struct plugin *plugin, struct client_session *cs,
                  struct session_request *sr)
{
    (void) plugin;
    char path[MAX_PATH_LEN];
    char uri[MAX_URI_LEN];

    struct cache_req_t *req = cache_reqs_get(cs->socket);
    if (req) {
        //printf("got back an old request, continuing stage 30!\n");
        return MK_PLUGIN_RET_CONTINUE;
    }

    struct cache_file_t *file = NULL;

    int uri_len = sr->uri_processed.len > MAX_URI_LEN ?
        MAX_URI_LEN : sr->uri_processed.len;

    int path_len = sr->real_path.len > MAX_PATH_LEN ?
        MAX_PATH_LEN : sr->real_path.len;

    strncpy(uri, sr->uri_processed.data, uri_len);
    strncpy(path, sr->real_path.data, path_len);
    uri[uri_len] = path[path_len] = '\0';



    if (sr->method == HTTP_METHOD_POST) {
        mk_info("got a post request! for urih: %s\n", uri);
        printf("Post data: %s\n\n", sr->data.data);

        return MK_PLUGIN_RET_END;
    }
    if (sr->method == HTTP_METHOD_GET) {
        file = cache_file_get(uri);
    }


    // check if its a call for the api's
    if (
        !file &&
        uri_len > API_PREFIX_LEN &&
        memcmp(uri, API_PREFIX, API_PREFIX_LEN) == 0
    ) {
        // REPLACE uri_ptr with offset
        mk_pointer uri_ptr = {
            .data = uri + API_PREFIX_LEN,
            .len = uri_len - API_PREFIX_LEN
        };

        if (uri_ptr.len >= 4 && memcmp(uri_ptr.data, "/add", 4) == 0 && sr->data.len) {
            printf("adding a new file with contents: %s\n", sr->data.data);
        }

        if (uri_ptr.len >= 6 && memcmp(uri_ptr.data, "/stats", 6) == 0) {
            return serve_stats(cs, sr);
        }

        if (uri_ptr.len >= 6 && memcmp(uri_ptr.data, "/webui", 6) == 0) {
            // remove the '/' as its already exists at the
            // end of conf_dir
            uri_ptr.data += 1;
            uri_ptr.len -= 1;

            mk_bug(conf_dir_len + uri_ptr.len >= MAX_PATH_LEN);

            memcpy(path, conf_dir, conf_dir_len);
            memcpy(path + conf_dir_len, uri_ptr.data, uri_ptr.len);

            path[conf_dir_len + uri_ptr.len] = '\0';

            file = cache_file_new(path, uri_ptr.data);
        }
    }

    if (!file) {
        file = cache_file_new(path, uri);
        if (!file) {
            mk_info("cant find the file, passing on the request :)");
            return MK_PLUGIN_RET_NOT_ME;
        }
    }

    req = cache_req_new();

    mk_bug(req->buf->filled != 0);

    req->socket = cs->socket;

    req->bytes_offset = 0;
    req->bytes_to_send = file->size;

    req->file = file;
    req->curr = mk_list_entry_first(&file->cache,
        struct pipe_buf_t, _head);

    // early fill of file request in caes it is not served
    fill_req_curr(req);

    if (req->buf->filled != 0) {
        mk_bug(req->buf->filled != 0);
    }

    mk_api->header_set_http_status(sr, MK_HTTP_OK);

    if (!file->cache_headers->filled) {
        // sr->headers.last_modified = sr->file_info.last_modification;
        sr->headers.content_length = file->size;
        sr->headers.real_length = file->size;
        sr->headers.content_type = get_mime(path);
        cache_file_fill_headers(file, cs, sr);
    }

    serve_cache_headers(req);

    // keep monkey plugin checks happy ;)
    sr->headers.sent = MK_TRUE;

    if (serve_req(req) <= 0) {

        cache_reqs_del(req->socket);
        return MK_PLUGIN_RET_END;
    }

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

