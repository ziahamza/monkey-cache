/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#include <sys/time.h>
#include "MKPlugin.h"
#include "cache_stats.h"

// to initialize the thread stats properly
pthread_mutex_t cache_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
int thread_num = 0;
void cache_stats_process_init() {
    thread_num = mk_api->config->workers;
    thread_stats = calloc(thread_num, sizeof(struct cache_stats_thread));
}

void cache_stats_thread_init() {
    static int wid = 0;

    pthread_mutex_lock(&cache_thread_mutex);
    mk_bug(wid == thread_num);
    printf("initializing thread %d\n", wid + 1);

    thread_stats[wid].index = wid;
    thread_stats[wid].reqs_per_sec = 0;
    thread_stats[wid].reqs_served = 0;
    gettimeofday(&thread_stats[wid].start, NULL);

    pthread_setspecific(cache_stats_thread_curr, (void *) &thread_stats[wid]);

    wid++;
    pthread_mutex_unlock(&cache_thread_mutex);
}

void cache_stats_thread_process(struct cache_stats_thread *stats) {
    struct timeval tmp;

    printf("updating the stats for thread %d / %d!\n", stats->index + 1, thread_num);
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
    printf("updating the global stats\n");

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
