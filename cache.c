/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */


#include <stdio.h>
#include <string.h>

#include <sys/mman.h>

#define __GNU_SOURCE
#define __USE_GNU

#include <fcntl.h>
#include <sys/ioctl.h>


#include "MKPlugin.h"
#include "cJSON.h"

#include "ht.h"
#include "utils.h"

#include "pipe_buf.h"
#include "cache_req.h"
#include "curr_reqs.h"
#include "cache_file.h"

#include "constants.h"

MONKEY_PLUGIN("cache",         /* shortname */
              "Monkey Cache", /* name */
              VERSION,        /* version */
              MK_PLUGIN_CORE_PRCTX | MK_PLUGIN_CORE_THCTX |  MK_PLUGIN_STAGE_30); /* hooks */

char conf_dir[MAX_PATH_LEN];
int conf_dir_len;

#define MIME_MAX_LEN 128

struct mime_map_t {
    // file extention
    char ext[MIME_MAX_LEN];

    // actual mime
    char mime[MIME_MAX_LEN];
};

struct buf_t mime_map;

int _mkp_init(struct plugin_api **api, char *confdir) {
    char config_path[MAX_PATH_LEN];

    mk_api = *api;
    strncpy(conf_dir, confdir, MAX_PATH_LEN);
    conf_dir_len = strlen(confdir);


    snprintf(config_path, MAX_PATH_LEN, "%scache.conf", confdir);
    struct mk_config *cnf = mk_api->config_create(config_path);

    buf_init(&mime_map, sizeof(struct mime_map_t), 16);
    struct mk_config_section *section = mk_api->config_section_get(cnf, "MIMETYPES");
    struct mk_list *mime_head;
    struct mk_config_entry *entry;

    int i = 0;
    struct mime_map_t tmp;
    mk_list_foreach(mime_head, &section->entries) {
        entry = mk_list_entry(mime_head, struct mk_config_entry, _head);

        strncpy(tmp.ext, entry->key, MIME_MAX_LEN);
        snprintf(tmp.mime, MIME_MAX_LEN, "%s\r\n", entry->val);
        buf_push(&mime_map, &tmp);
        i++;
    }

    mk_api->config_free(cnf);
    return 0;
}

void _mkp_exit() {
    // TODO: free curr_reqs
    cache_file_exit();
    pipe_buf_exit();
    cache_req_exit();
}

int _mkp_core_prctx(struct server_config *config) {
    (void) config;

    mk_info("Started Monkey Cache plugin");
    pthread_key_create(&curr_reqs, NULL);

    cache_req_process_init();
    pipe_buf_process_init();
    cache_file_process_init();
    return 0;
}
void _mkp_core_thctx() {

    curr_reqs_init();
    cache_req_thread_init();
    pipe_buf_thread_init();
    cache_file_thread_init();
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

    if (ret < req->file->header_len) {

        printf("teed %d data instead of headers len %ld\n", ret, req->file->header_len);
        mk_bug(ret < req->file->header_len);
    }

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
    struct cache_req_t *req = curr_reqs_get(fd);
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

        curr_reqs_del(fd);

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
    const mk_pointer mime = mk_pointer_init("text/plain\r\n");
    int i, j;

    int len = strlen(path);
    for (j = len; j > 0; j--) {
        if (path[j - 1] != '.')
            continue;

        for (i = 0; i < mime_map.size; i++) {
            struct mime_map_t *m = buf_get(&mime_map, i);
            if (strcmp(path + j, m->ext) == 0) {
                mk_pointer tmp = {
                    .data = m->mime,
                    .len = strlen(m->mime)
                };

                return tmp;
            }
        }
        break;
    }

    return mime;
}

void fill_cache_headers(struct cache_file_t *file,
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


int serve_str(struct client_session *cs, struct session_request *sr, char *str) {

    mk_api->header_set_http_status(sr, MK_HTTP_OK);

    sr->headers.content_length = strlen(str);
    sr->headers.content_type = get_mime(".txt");

    mk_api->header_send(cs->socket, cs, sr);

    mk_api->socket_send(cs->socket, str, strlen(str));
    return MK_PLUGIN_RET_END;
}

int serve_reset(struct client_session *cs, struct session_request *sr, char *uri) {
    cache_file_reset(uri);
    return serve_str(cs, sr, "Cache Reset Successfully!\n");
}

int serve_add(struct client_session *cs, struct session_request *sr, char *uri) {
    cache_file_tmp(uri, &sr->data);
    return serve_str(cs, sr, "Cache resource added sucessfully!\n");
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
    cJSON_AddNumberToObject(mem,"pipe_mem_used", pipe_buf_mem_used());

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
int _mkp_stage_30(struct plugin *plugin, struct client_session *cs,
                  struct session_request *sr)
{
    (void) plugin;
    char path[MAX_PATH_LEN];
    char uri[MAX_URI_LEN];

    struct cache_req_t *req = curr_reqs_get(cs->socket);
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

        if (uri_ptr.len >= 5 && memcmp(uri_ptr.data, "/add/", 5) == 0 && sr->data.len) {
            uri_ptr.data += 4;
            uri_ptr.len -= 4;

            strncpy(path, uri_ptr.data, uri_ptr.len);
            path[uri_ptr.len] = '\0';

            return serve_add(cs, sr, path);
        }

        if (uri_ptr.len >= 6 && memcmp(uri_ptr.data, "/stats", 6) == 0) {
            return serve_stats(cs, sr);
        }

        if (uri_ptr.len >= 7 && memcmp(uri_ptr.data, "/reset/", 7) == 0) {
            uri_ptr.data += 6;
            uri_ptr.len -= 6;

            strncpy(path, uri_ptr.data, uri_ptr.len);
            path[uri_ptr.len] = '\0';

            return serve_reset(cs, sr, path);
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

            // -1 for including the backslash
            file = cache_file_new(path, uri_ptr.data - 1);
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
    curr_reqs_add(req);

    req->socket = cs->socket;

    req->bytes_offset = 0;
    req->bytes_to_send = file->size;

    req->file = file;
    req->curr = mk_list_entry_first(&file->cache,
        struct pipe_buf_t, _head);

    // early fill of file request in caes it is not served
    fill_req_curr(req);

    mk_bug(req->buf->filled != 0);

    mk_api->header_set_http_status(sr, MK_HTTP_OK);

    if (!file->cache_headers->filled) {
        // sr->headers.last_modified = sr->file_info.last_modification;
        sr->headers.content_length = file->size;
        sr->headers.real_length = file->size;
        sr->headers.content_type = get_mime(path);
        fill_cache_headers(file, cs, sr);
    }

    serve_cache_headers(req);

    // keep monkey plugin checks happy ;)
    sr->headers.sent = MK_TRUE;

    if (serve_req(req) <= 0) {

        curr_reqs_del(req->socket);
        return MK_PLUGIN_RET_END;
    }

    return MK_PLUGIN_RET_CONTINUE;
}


int _mkp_event_error(int socket)
{
    mk_info("got an error with a socket!");
    curr_reqs_del(socket);
    return MK_PLUGIN_RET_EVENT_NEXT;
}
int _mkp_event_timeout(int socket)
{
    mk_info("got an error with a socket!");
    curr_reqs_del(socket);
    return MK_PLUGIN_RET_EVENT_NEXT;
}

