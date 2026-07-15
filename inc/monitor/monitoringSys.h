#ifndef __INC_MONITOR_MONITORINGSYS_H__
#define __INC_MONITOR_MONITORINGSYS_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif


#ifdef __cplusplus
extern "C"{
#endif

// Minial logging stub in the real framework    
#define SM_LOG(fmt, ...) fprintf(stderr, "[sysmon] " fmt "\n", ##__VA_ARGS__)

// process init contract 
typedef struct {
    const char *smi_name;       // process name
    uint16_t    smi_cli_port;   // CLI debug port
    uint8_t     smi_sw_id;      // software id
} sys_mon_init_t;

void *sys_mon_init(const sys_mon_init_t *sm_init, int argc,char *argv[]);
// arg = whatever init returned
int   sys_mon_run(void *gsm);
void  sys_mon_stop(void);

// callback contract 
typedef int (SysMonCallbackFunc_t)(void *arg[]);
typedef struct sm_cb_s{
    const char *cb_name;
    SysMonCallbackFunc_t *cb_callback;
    void *cb_arg[4];
} SysMonCallback_t;

#define SM_TIMER_ONCE 0x00     // fire once after delay
#define SM_TIMER_RECURE 0x01   // fire every delay ms
void *sys_mon_timer_set(SysMonCallback_t *sm_cb, uint32_t flags, int32_t ms_delay);

// async I/O contract
// Register fd with the event loop; cb fires when fd becomes readable.
// returns an opaque handle (pass to aio_remove), or NULL on failure.
void *sys_mon_aio_add(SysMonCallback_t *sm_cb,int fd);
int sys_mon_aio_remove(void *handle);


// data model (stable; used once DB/report layer lands)
#define SM_DESCR_SIZE 64
typedef struct sm_value_s{
    uint32_t sv_mid;    // module id 
    uint32_t sv_hid;    // hardware id 
    uint32_t sv_eid;    // error/metric id 
    uint32_t sv_lvl;    // severity level
    char     sv_descr[SM_DESCR_SIZE];
    int      sv_type;   // 0 int, 1 string
    uint32_t sv_num;
} SysMonData_t;

#ifdef __cplusplus
}
#endif // extern "C"

#endif // __INC_MONITOR_MONITORINGSYS_H__
