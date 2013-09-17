/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */


#include <stdio.h>
#include <string.h>
#include <math.h>

#include <sys/mman.h>
#include <sys/time.h>

#define __GNU_SOURCE
#define __USE_GNU

#include <fcntl.h>
#include <sys/ioctl.h>


#include "MKPlugin.h"
#include "cJSON.h"

#include "ht.h"
#include "utils.h"
#include "socket.h"
#include "mime_map.h"

#include "pipe_buf.h"
#include "cache_req.h"
#include "curr_reqs.h"
#include "cache_file.h"

#include "constants.h"

MONKEY_PLUGIN("cache",         /* shortname */
              "Monkey Cache", /* name */
              VERSION,        /* version */
              MK_PLUGIN_CORE_PRCTX | MK_PLUGIN_CORE_THCTX |  MK_PLUGIN_STAGE_30); /* hooks */

// stats averaged over a second
struct {
    double avg_reqs_served;
} stats;

struct {
    struct timeval start;
    int valid;
    int reqs_served;
    pthread_mutex_t update_time;
} cache_stats;

void cache_stats_init() {
    cache_stats.valid = 1;
    cache_stats.reqs_served = 0;
    gettimeofday(&cache_stats.start, NULL);
    pthread_mutex_init(&cache_stats.update_time, NULL);
}

void cache_stats_update() {
    struct timeval tmp;
    int ms = 0;
    gettimeofday(&tmp, NULL);

    ms += (tmp.tv_sec - cache_stats.start.tv_sec) * 1000.0;
    ms += (tmp.tv_usec - cache_stats.start.tv_usec) / 1000.0;
    if (ms > 1000) {
        cache_stats.start = tmp;
        stats.avg_reqs_served = cache_stats.reqs_served / (ms / 1000.0);

        cache_stats.reqs_served = 0;
    }
}

char conf_dir[MAX_PATH_LEN];
int conf_dir_len;

int _mkp_init(struct plugin_api **api, char *confdir) {
    cache_stats_init();
    char config_path[MAX_PATH_LEN];

    mk_api = *api;
    strncpy(conf_dir, confdir, MAX_PATH_LEN);
    conf_dir_len = strlen(confdir);


    snprintf(config_path, MAX_PATH_LEN, "%scache.conf", confdir);
    config_path[MAX_PATH_LEN - 1] = '\0';

    struct mk_config *cnf = mk_api->config_create(config_path);

    mime_map_init(cnf);

    mk_api->config_free(cnf);
    return 0;
}

void _mkp_exit() {
    cache_file_exit();
    pipe_buf_exit();
    cache_req_exit();
    // TODO: free currest requests
    // curr_reqs_exit();
}

struct server_config *config;
int _mkp_core_prctx(struct server_config *conf) {

    mk_info("Started Monkey Cache plugin");
    pthread_key_create(&curr_reqs, NULL);

    cache_req_process_init();
    pipe_buf_process_init();
    cache_file_process_init();

    config = conf;
    return 0;
}
void _mkp_core_thctx() {

    curr_reqs_init();
    cache_req_thread_init();
    pipe_buf_thread_init();
    cache_file_thread_init();
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
        return socket_serve_req_fd(req);
    }

    return socket_serve_req_splice(req);
}

int _mkp_event_read(int fd) {
    (void) fd;
    return MK_PLUGIN_RET_EVENT_CONTINUE;
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

// TODO: Assuming headers are only filled once
void fill_cache_headers(struct cache_file_t *file,
    struct client_session *cs, struct session_request *sr) {
    int ret = 0;
    pthread_mutex_lock(&file->cache_headers->write_mutex);
    if (!file->cache_headers->filled) {

        // HACK: change server request values to prevent monkey to
        // not add "connection: close" in any case as it messes up things
        // when served every request with same cached headers.
        int old_conn = sr->headers.connection,
            old_keepalive = sr->keep_alive,
            old_close = sr->close_now,
            old_conlen = sr->connection.len;

        sr->headers.connection= 0;
        sr->keep_alive = MK_TRUE;
        sr->close_now = MK_FALSE;

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
    sr->headers.content_type = mime_map_get(".txt");

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

    cJSON *root, *mem, *reqs, *files, *file;
    char *out;

    root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "memory", mem = cJSON_CreateObject());

    // size converted to MB
    cJSON_AddNumberToObject(mem,"pipe_size", PIPE_SIZE / (1024.0 * 1024.0));
    cJSON_AddNumberToObject(mem,"pipe_mem_used", pipe_buf_mem_used() / (1024.0 * 1024.0));

    cJSON_AddItemToObject(root, "requests", reqs = cJSON_CreateObject());
    cJSON_AddNumberToObject(reqs, "served_per_sec", ceil(stats.avg_reqs_served));


    cJSON_AddItemToObject(root, "files", files = cJSON_CreateArray());

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
    sr->headers.content_type = mime_map_get(".json");

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

    pthread_mutex_lock(&cache_stats.update_time);
    cache_stats_update();
    cache_stats.reqs_served++;
    pthread_mutex_unlock(&cache_stats.update_time);

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
    __sync_fetch_and_add(&req->file->pending_reqs, 1);

    req->curr = mk_list_entry_first(&file->cache,
        struct pipe_buf_t, _head);

    // early fill of file request in caes it is not served
    cache_req_fill_curr(req);

    mk_bug(req->buf->filled != 0);

    mk_api->header_set_http_status(sr, MK_HTTP_OK);
    sr->headers.content_length = file->size;
    sr->headers.real_length = file->size;
    sr->headers.content_type = mime_map_get(path);


    if (!file->cache_headers->filled) {
        // sr->headers.last_modified = sr->file_info.last_modification;
        fill_cache_headers(file, cs, sr);
    }

    PLUGIN_TRACE("conn done: %d", config->max_keep_alive_request - cs->counter_connections);
    if ((config->max_keep_alive_request - cs->counter_connections) <= 0) {
        mk_api->header_send(cs->socket, cs, sr);
    }
    else {
        serve_cache_headers(req);
    }

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

