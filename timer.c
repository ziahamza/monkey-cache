/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#include <sys/timerfd.h>
#include "timer.h"

#include "cache_stats.h"

int timer_fd;

int timer_get_fd() {
    return timer_fd;
}

void timer_thread_init() {
    int epoll_fd = mk_api->sched_worker_info()->epoll_fd;
    mk_api->epoll_add(epoll_fd, timer_fd, MK_EPOLL_READ, MK_EPOLL_EDGE_TRIGGERED);
}

void timer_read() {
    char time[8];
    int cnt = 0;
    if ((cnt = read(timer_fd, time, 8)) == 8) {
        // handle interval tasks!
        cache_stats_tick();
    }
}

void timer_process_init() {
    struct itimerspec timeout;
    struct timespec ts;
    ts.tv_nsec = 0;
    ts.tv_sec = 1;

    timeout.it_interval = ts;
    timeout.it_value = ts;

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (timerfd_settime(timer_fd, 0, &timeout, NULL) < 0) {
        perror("setting timerfd failed!\n");
    }
}
