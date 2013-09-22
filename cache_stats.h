/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#ifndef __CACHE_STATS_H_
#define __CACHE_STATS_H_

// the struct to keep track of statistics of this thread
pthread_key_t cache_stats_thread_curr;

// global statistics
struct {
    int reqs_per_sec;
} cache_stats;

struct cache_stats_thread {
    // the index of stat in thread_stats
    int index;

    double reqs_per_sec;

    // dirty stats from the start timeval
    struct timeval start;
    int reqs_served;
};

struct cache_stats_thread *thread_stats;

void cache_stats_process_init();
void cache_stats_thread_init();

// a global tick after every specific timeout,
// used to update the global statistics
void cache_stats_tick();


void cache_stats_req_new();

#endif
