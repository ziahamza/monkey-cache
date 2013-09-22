#include "curr_reqs.h"

void curr_reqs_process_init() {
    pthread_key_create(&curr_reqs, NULL);
}
void curr_reqs_thread_init() {

    struct mk_list *reqs = malloc(sizeof(struct mk_list));
    mk_list_init(reqs);
    pthread_setspecific(curr_reqs, reqs);
}

struct cache_req_t *curr_reqs_get(int socket) {
    struct mk_list *reqs = pthread_getspecific(curr_reqs);
    struct mk_list *curr;

    mk_list_foreach(curr, reqs) {
        struct cache_req_t *req = mk_list_entry(curr, struct cache_req_t, _head);
        if (req->socket == socket) {
            return req;
        }
    }

    return NULL;
}

void curr_reqs_add(struct cache_req_t *req) {
    struct mk_list *reqs = pthread_getspecific(curr_reqs);
    mk_list_add(&req->_head, reqs);
}

void curr_reqs_del(int socket) {
    struct mk_list *reqs = pthread_getspecific(curr_reqs);
    struct mk_list *curr, *next;

    mk_list_foreach_safe(curr, next, reqs) {
        struct cache_req_t *req = mk_list_entry(curr, struct cache_req_t, _head);
        if (req->socket == socket) {
            cache_req_del(req);
            return;
        };
    }
}

void curr_reqs_exit() {
    struct mk_list *reqs = pthread_getspecific(curr_reqs);
    struct mk_list *curr, *next;

    mk_list_foreach_safe(curr, next, reqs) {
        struct cache_req_t *req = mk_list_entry(curr, struct cache_req_t, _head);
        cache_req_del(req);
    }

    /*
    free(reqs);
    pthread_setspecific(curr_reqs, NULL);
    */
}
