#include "monitor/monitoringSys.h"
#include "monitor/montoringTimer.h"
#include "monitor/private_sm.h" // M3 : SysMonCB_Enh_t sm_activate_cb, sm_link_cb

#include <limits.h>
#include <pthread.h>
#include <stdlib.h> 
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>

typedef struct sm_timer_s {
    struct sm_timer_s *next;
    struct timeval     next_time; // when to fire next (monotonic)
    struct timeval     delay;     // 0.0 ==> one-shot 
    /* SysMonCallback_t   cb; */  // M2
    /*
     * M3 : SysMonCB_Enh_t was SysMonCallback_t 
     * now the enhanced wrapper so activation goes 
     * through the shared stats/thread path
     * */
    SysMonCB_Enh_t     cb;        
    /* uint32_t           flags; */ // M2
    unsigned int       fire_count;  // simple stat
} SysMonTimer_t;

/*
 * The whole subsystem stat: just the head of the ordered list
 * (The real framework guards this with a mutex ; single-threaded M1)
 * doesn't need one yet - that arrives with the thread-mode milestone.
 * */
static SysMonTimer_t *g_earliest = NULL;

static void mono_now(struct timeval *now){
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    now->tv_sec = tp.tv_sec;
    now->tv_usec = tp.tv_nsec / 1000;
}

// Insert keeping the list sorted by next_time ascending.
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

    // M3: fill the enhanced wrapper instead of a bare callback copy
    // M3: embed the user callback 
    t->cb.sme_cb = *sm_cb;
    // M3: so first sample sets the min 
    t->cb.sme_time_min = UINT_MAX;
    t->cb.sme_time_max = 0;
    // M3: opt into thread execution
    if(flags & SM_TIMER_THREAD){
        t->cb.sme_flags |= SMECB_ASYNCH;
        pthread_mutex_init(&t->cb.sme_mutex, NULL);
        pthread_cond_init(&t->cb.sme_cond, NULL);
    }
    sm_link_cb(&t->cb);

    t->delay.tv_sec = ms_delay / 1000;
    t->delay.tv_usec = (ms_delay % 1000) *1000;

    struct timeval now;
    mono_now(&now);
    // first deadline = now + delay
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

    // fire everything that is already due 
    while(g_earliest && timercmp(&g_earliest->next_time, &now, <)){
        SysMonTimer_t *t = g_earliest;
        // pop the earliest
        g_earliest = t->next;
        t->next = NULL;

        t->fire_count++;
        /*
         * M3: was a direct (*t->cb.cb_callback)(...) call, Now every
         * activation goes through sm_activate_cb so timing stats are
         * recorded and SM_TIMER_THREAD callbacks run off the loop.
         * */
        sm_activate_cb(&t->cb);

        // --------------------- M2 -----------------------------
        /* if(t->cb.cb_callback){ */
        /*     (void)(*t->cb.cb_callback)(t->cb.cb_arg); */
        /* } */

        // recurring 
        if(t->delay.tv_sec || t->delay.tv_usec){
            // re-arm 
            timeradd(&t->next_time, &t->delay, &t->next_time);
            insert_timer(t);
        }
        // one-shot
        else{
            // M3 : drop from  the stats registry first
            sm_unlink_cb(&t->cb);
            free(t);
        }

        // callbacks take time; refresh before re-checking
        mono_now(&now);
    }

    // Report how long the caller may sleep until the next deadline.
    if (g_earliest){
        mono_now(&now);
        timersub(&g_earliest->next_time, &now, sleep);
        // already overdue -> dont block
        if(sleep->tv_sec < 0){
            sleep->tv_sec = 0;
            sleep->tv_usec = 0;
        }
        else{
            // idle: wake up periodically
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
        // M3: remove from stats registry before free
        sm_unlink_cb(&t->cb);
        free(t);
        t = n;
    }
    g_earliest = NULL;
}
