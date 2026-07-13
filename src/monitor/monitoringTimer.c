#include "monitor/monitoringSys.h"
#include "monitor/monttoringTimer.h"

#include <stddef.h>
#include <stdlib.h> 
#include <string.h>
#include <sys/time.h>
#include <time.h>

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


