#include "monitor/monitoringSys.h"
#include "monitor/montoringTimer.h"

#include <bits/types/struct_timeval.h>
#include <stddef.h>
#include <stdlib.h> 
/* #include <string.h> */
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct sm_timer_s {
    struct sm_timer_s *next;
    struct timeval     next_time;
    struct timeval     delay;
    SysMonCallback_t   cb;
    uint32_t           flags;
    unsigned int       fire_count;
} SysMonTimer_t;

static SysMonTimer_t *g_earliest = NULL;

static void mono_now(struct timeval *now){
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    now->tv_sec = tp.tv_sec;
    now->tv_usec = tp.tv_nsec / 1000;
}

static void insert_timer(SysMonTimer_t *t){
    SysMonTimer_t **pp = &g_earliest;
    for(; *pp; pp = &(*pp)->next){
        if(timercmp(&t->next_time, &(*pp)->next_time, <)){
            break;
        }
    }
    t->next = *pp;
    *pp = t;
}


void *sys_mon_timer_set(SysMonCallback_t *sm_cb, uint32_t flags, int32_t ms_delay){
    SysMonTimer_t *t = (SysMonTimer_t*)calloc(1, sizeof(*t));
    if (!t) {
        SM_LOG("timer set: calloc failed");
        return NULL;
    }

    t->cb = *sm_cb;

    t->delay.tv_sec = ms_delay / 1000;
    t->delay.tv_usec = (ms_delay % 1000) *1000;

    struct timeval now;
    mono_now(&now);
    timeradd(&now, &t->delay, &t->next_time);

    if(0 == (flags & SM_TIMER_RECURE)) {
        t->delay.tv_sec = t->delay.tv_usec = 0;
    }

    insert_timer(t);
    return t;
}

int sys_mon_timer_check(struct timeval *sleep){
    struct timeval now;
    mono_now(&now);

    while(g_earliest && timercmp(&g_earliest->next_time, &now, <)){
        SysMonTimer_t *t = g_earliest;
        // pop the earliest
        g_earliest = t->next;
        t->next = NULL;

        t->fire_count++;
        if(t->cb.cb_callback){
            (void)(*t->cb.cb_callback)(t->cb.cb_arg);
        }

        if(t->delay.tv_sec || t->delay.tv_usec){
            timeradd(&t->next_time, &t->delay, &t->next_time);
            insert_timer(t);
        }else{
            free(t);
        }

        mono_now(&now);
    }

    if (g_earliest){
        mono_now(&now);
        timersub(&g_earliest->next_time, &now, sleep);
        if(sleep->tv_sec < 0){
            sleep->tv_sec = 0;
            sleep->tv_usec = 0;
        }
        else{
            sleep->tv_sec = 1;
            sleep->tv_usec = 0;
        }
    }
    return 0;
}

void sys_mon_timer_cleanup(){
    SysMonTimer_t *t = g_earliest;
    while(t){
        SysMonTimer_t *n = t->next;
        free(t);
        t = n;
    }
    g_earliest = NULL;
}
