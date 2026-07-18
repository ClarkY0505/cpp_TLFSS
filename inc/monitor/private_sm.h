#ifndef __INC_MONITOR_PRIVATE_SM_H__
#define __INC_MONITOR_PRIVATE_SM_H__

#include "monitor/monitoringSys.h"
#include <limits.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C"{
#endif

/* ============================= M2 ===================================
 * Internal shared declarations (not part of the public contract).
 * sm_wakeup lives in the core but is called from other subsystems to 
 * interrupt a blocked select() via the self-pipe
 * */
void sm_wakeup(void);

/* ============================= M3 ===================================
 * Enhanced callback: the ONE object both event sources (timers and 
 * aio) wrap their user callback in, Before M3, timer_check and aio_proccess
 * each called cb.cb_callback() directly and inline.That meant per-callback 
 * stats and "run in a thread" would have to be written twice. This struct + 
 * sm_activate_cb() centralize both.
 *
 * Layout: it embeds the user's SysMonCallback_t and adds 
 *  - execution mode (inline vs its own thread)
 *  - timing stats (count / min / max / total, in microseconds)
 *  - a link so every enhanced callback is discoverable in one list 
 * ==================================================================== */

typedef struct SysMonCB_Enh_s{
    // the wrapped user callback 
    SysMonCallback_t sme_cb;
    // shorthands so call sites read like the real framework's
#define sme_name     sme_cb.cb_name
#define sme_callback sme_cb.cb_callback 
#define sme_arg      sme_cb.cb_arg

    int sme_flags;
#define SMECB_ASYNCH 0x01

    // thread-mode plumbing (only used when SMECB_ASYNCH is set)
    pthread_t       sme_thread;
    pthread_cond_t  sme_cond;
    pthread_mutex_t sme_mutex;

    // per-callback timing statistics (all in microseconds)
    unsigned int    sme_count;
    unsigned long   sme_time_total;
    unsigned int    sme_time_min;
    unsigned int    sme_time_max;

    struct SysMonCB_Enh_s *sme_next;
} SysMonCB_Enh_t;

/* single activation path used by BOTH timers and aio , Times the
 * call, updates stats, and dispatches inline or to a worker thread. */
int sm_activate_cb(SysMonCB_Enh_t *ecb);
// register / unregister in the global stats list
void sm_link_cb(SysMonCB_Enh_t *ecb);
void sm_unlink_cb(SysMonCB_Enh_t *ecb);
// signals worker threads to exit during cleanup
void sm_stop_threads(void);
// dump per-callback stats (called from run() on the way out)
void sm_print_stats_all_cb(void);

#ifdef __cplusplus
}
#endif

#endif // __INC_MONITOR_PRIVATE_SM_H__
