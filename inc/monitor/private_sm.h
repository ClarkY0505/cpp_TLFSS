#ifndef __INC_MONITOR_PRIVATE_SM_H__
#define __INC_MONITOR_PRIVATE_SM_H__

/*
 * Internal shared declarations (not part of the public contract).
 * sm_wakeup lives in the core but is called from other subsystems to 
 * interrupt a blocked select() via the self-pipe
 * */
void sm_wakeup(void);

#endif // __INC_MONITOR_PRIVATE_SM_H__
