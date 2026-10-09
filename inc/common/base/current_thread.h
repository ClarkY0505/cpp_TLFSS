#ifndef __INC_COMMON_BASE_CURRENT_THREAD_H__
#define __INC_COMMON_BASE_CURRENT_THREAD_H__
#include <unistd.h>
#include <sys/syscall.h>
#include <string>

namespace TLSS::BASE::CurrentThread {
extern __thread int t_cached_tid;
void cache_tid();
inline int tid(){
    //
    // __builtin_expect 这里主要的作用是预测分支，
    // 减少CPU负担
    // 这里预测的是t_cached_tid == 0 这个值是0
    // 也就是t_cached_tid != 0
    if(__builtin_expect(t_cached_tid == 0, 0)){
        cache_tid();
    }
    return t_cached_tid;
}

std::string stack_trace(bool demangle);

}  // namespace TLSS::BASE::CurrentThread

#endif  // __INC_COMMON_BASE_CURRENT_THREAD_H__
