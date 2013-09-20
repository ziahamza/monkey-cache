/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#include <sys/timerfd.h>
#include "timer.h"

int timer_fd;
void timer_process_init() {
    struct itimerspec timeout;
    struct timespec ts;
    ts.tv_nsec = 0;
    ts.tv_sec = 1;

    timeout.it_interval = ts;
    timeout.it_value = ts;

    struct sched_list_node *sched = mk_sched_get_thread_conf();
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (timerfd_settime(timer_fd, 0, &timeout, NULL) < 0) {
        perror("setting timerfd failed!\n");
    }

    printf("set the timer fd %d\n", timer_fd);

    mk_api->epoll_add(sched->epoll_fd, timer_fd, MK_EPOLL_READ, MK_EPOLL_LEVEL_TRIGGERED);
}
