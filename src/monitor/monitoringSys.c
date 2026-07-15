#include "monitor/monitoringSys.h"
#include "monitor/montoringTimer.h"
#include "monitor/private_sm.h"

#include <asm-generic/errno-base.h>
#include <bits/types/struct_timeval.h>
#include <linux/stat.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>

typedef struct SysMonAIO_s{
    struct SysMonAIO_s *next;
    int fd;
    int flags;
#define SMA_DELAY_DEL 0x01
    SysMonCallback_t cb;
} SysMonAIO_t;

typedef struct{
    char name[32];
    bool runninng;
    SysMonAIO_t *aio;
    int pipe[2];
} SysMonCommon_t;

// global contest (as in the original)
static SysMonCommon_t *gsm = NULL;

// self-pipe : make the blocked select() return right now
void sm_wakeup(void){
    if(gsm && write(gsm->pipe[1], "*", 1) != 1){
        SM_LOG("sm_wakeup: write failed: %s", strerror(errno));
    }
}

static int wakeup_cb(void *arg[]){
    int *fd = (int *)arg[0];
    char buf[16];
    (void)read(*fd, buf, sizeof(buf));
    return 0;
}

void *sys_mon_aio_add(SysMonCallback_t *sm_cb, int fd){
    SysMonAIO_t *aio = (SysMonAIO_t*)calloc(1, sizeof(*aio));
    if (!aio){
        SM_LOG("aio_add: calloc failed");
        return NULL;
    }
    aio->fd = fd;
    aio->cb = *sm_cb;
    gsm->aio = aio;
    sm_wakeup();
    return aio;
}

int sys_mon_aio_remove(void *handle){
    if(!handle){
        return -1;
    }
    /*
     * Lazy delete: only flag it. The process loop is (or may be )walking
     * this list; freeing here could pull the rug out. it fress on next pass.
     * */
    ((SysMonAIO_t*)handle)->flags |= SMA_DELAY_DEL;
    sm_wakeup();
    return 0;
}

/*
 * Build the fd_set , select() with timeout=*sleep , fire readable fds.
 * Also reaps nodes flagged SMA_DELAY_DEL.
 * */
static int sys_mon_aio_process(struct timeval *sleep){
    fd_set fds;
    int nfds;

    FD_ZERO(&fds);

    // pass 1: reap delayed-deletes, arm the rest
    for(SysMonAIO_t **pp = &gsm->aio; *pp;){
        SysMonAIO_t *aio = *pp;
        if(aio->flags & SMA_DELAY_DEL){
            *pp = aio->next;
            free(aio);
            continue;
        }
        FD_SET(aio->fd, &fds);
        if(aio->fd > nfds){
            nfds = aio->fd;
        }

        pp = &aio->next;
    }

    int rc = select(nfds + 1, &fds, NULL, NULL, sleep);
    if(rc < 0){
        if(errno != EINTR){
            SM_LOG("aio_process: select failed: %s", strerror(errno));
        }
        return -1;
    }
    // pass 2: fire the readable ones
    for(SysMonAIO_t *aio = gsm->aio; aio; aio = aio->next){
        if ((aio->flags & SMA_DELAY_DEL) == 0 && FD_ISSET(aio->fd, &fds)){
            if(aio->cb.cb_callback){
                (void)(*aio->cb.cb_callback)(aio->cb.cb_arg);
            }
        }
    }
    return 0;
}


void *sys_mon_init(const sys_mon_init_t *sm_init, int argc,char *argv[]){
    UNUSED(argc);
    UNUSED(argv);
    gsm = (SysMonCommon_t*)calloc(1, sizeof(*gsm));
    if(!gsm){
        SM_LOG("init: calloc faild");
        return NULL;
    }
    strncpy(gsm->name, sm_init->smi_name, sizeof(gsm->name) - 1);

    // self-pipe, with its read end registered as the firest aio fd
    if(pipe(gsm->pipe) != 0){
        SM_LOG("init: pipe failed: %s", strerror(errno));
        free(gsm);
        gsm = NULL;
        return NULL;
    }

    static SysMonCallback_t wake = {.cb_name = "wakeup", .cb_callback = wakeup_cb};
    wake.cb_arg[0] = &gsm->pipe[0];
    sys_mon_aio_add(&wake, gsm->pipe[0]);

    SM_LOG("System Monitor '%s' initialized (cli_port=%u)",
           gsm->name, sm_init->smi_cli_port);
    return gsm;
}

static void do_sleep(struct timeval *sleep){
    select(0, NULL, NULL, NULL, sleep);
}

int sys_mon_run(void *arg){
    SysMonCommon_t *g = (SysMonCommon_t*)arg;
    if(!g){
        return -1;
    }

    g->runninng = true;
    while(g->runninng){
        struct timeval sleep;
        // pass 1: fire due timers , compute sleep
        sys_mon_timer_check(&sleep); 
        /* do_sleep(&sleep); */
        // pass 2: select(timemout = sleep ), fire fds
        sys_mon_aio_process(&sleep);
    }

    sys_mon_timer_cleanup();
    SM_LOG("System Monitor '%s' stopped", g->name);

    // free remaining aio nodes + pipe
    for(SysMonAIO_t * aio = g->aio; aio ;){
        SysMonAIO_t *n = aio->next;
        free(aio);
        aio = n;
    }

    close(g->pipe[0]);
    close(g->pipe[1]);
    free(g);
    gsm = NULL;
    return 0;
}

void sys_mon_stop(){
    if(gsm){
        gsm->runninng = false;
        // break out of a blocked select() so the loop exits now 
        sm_wakeup();
    }
}
