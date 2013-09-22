#ifndef __TIMER_H_
#define __TIMER_H_

#include "MKPlugin.h"

void timer_thread_init();
void timer_process_init();

int timer_get_fd();
void timer_read();

#endif
