#ifndef __INC_MONITOR_MONTTORINGTIMER_H__
#define __INC_MONITOR_MONTTORINGTIMER_H__

/*
 * Timer subsystem - internal interface (used by the core run loop).
 * */

#include <sys/time.h>

#ifdef __cplusplus
extern "C"{
#endif

/*
 * Fire every time whose deadline has passed, re-arm recurring ones,
 * then write "time remaining until the next timer" into *sleep
 * That sleep value is how the timer half tells the I/O half how long
 * it may block.
 * */
int sys_mon_timer_check(struct timeval *sleep);

// Release all timer nodes on the way out.
void sys_mon_timer_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // __INC_MONITOR_MONTTORINGTIMER_H__
