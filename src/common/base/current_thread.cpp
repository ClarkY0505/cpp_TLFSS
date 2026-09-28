#include "common/base/current_thread.h"

namespace TLSS::BASE::CurrentThread{
__thread int t_cached_tid = 0;

void cache_tid(){
    if(t_cached_tid == 0){
        // 
        // 通过系统调用，获取当前线程的tid值
        t_cached_tid = static_cast<pid_t>(::syscall(SYS_gettid));
    }
}
}
