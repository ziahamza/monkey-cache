/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#include <sys/time.h>
#include "MKPlugin.h"
#include "cache_stats.h"

// to initialize the thread stats properly
pthread_mutex_t cache_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

// the struct to keep track of statistics of this thread
pthread_key_t cache_stats_thread_curr;

int thread_num = 0;
void cache_stats_process_init() {
    pthread_key_create(&cache_stats_thread_curr, NULL);
    thread_num = mk_api->config->workers;
    thread_stats = calloc(thread_num, sizeof(struct cache_stats_thread));
}

static int wid = 0;
void cache_stats_thread_init() {

    pthread_mutex_lock(&cache_thread_mutex);
    mk_bug(wid >= thread_num);

    struct cache_stats_thread *stats = &thread_stats[wid];

    stats->index = wid;
    stats->reqs_per_sec = 0;
    stats->reqs_served = 0;
    gettimeofday(&stats->start, NULL);

    pthread_setspecific(cache_stats_thread_curr, (void *) stats);

    wid++;
    pthread_mutex_unlock(&cache_thread_mutex);
}

void cache_stats_thread_process(struct cache_stats_thread *stats) {
    struct timeval tmp;
    int ms = 0;
    gettimeofday(&tmp, NULL);

    ms += (tmp.tv_sec - stats->start.tv_sec) * 1000.0;
    ms += (tmp.tv_usec - stats->start.tv_usec) / 1000.0;
    if (ms > 1000) {
        stats->start = tmp;
        stats->reqs_per_sec = stats->reqs_served / (ms / 1000.0);
        stats->reqs_served = 0;
    }
}

void cache_stats_tick() {
    int reqs_per_sec = 0;
    for (int i = 0; i < thread_num; i++) {
        cache_stats_thread_process(&thread_stats[i]);
        reqs_per_sec += thread_stats[i].reqs_per_sec;
    }

    cache_stats.reqs_per_sec = reqs_per_sec;
}

// called for each new request
void cache_stats_req_new() {
    struct cache_stats_thread *stats = pthread_getspecific(cache_stats_thread_curr);
    stats->reqs_served++;
}
