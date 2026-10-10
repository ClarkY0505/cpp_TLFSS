#include <cxxabi.h>
#include <execinfo.h>
#include <stdlib.h>
#include <string>
#include "common/base/current_thread.h"

namespace TLSS::BASE::CurrentThread {
__thread int t_cached_tid = 0;

void cache_tid() {
  if (t_cached_tid == 0) {
    //
    // 通过系统调用，获取当前线程的tid值
    t_cached_tid = static_cast<pid_t>(::syscall(SYS_gettid));
  }
}

std::string stack_trace(bool demangle) {
  std::string stack;
  const int max_frames = 200;
  void* frame[max_frames];
  int nptrs = ::backtrace(frame, max_frames);
  char** strings = ::backtrace_symbols(frame, nptrs);
  if (strings) {
    size_t len = 256;
    char* demangled = demangle ? static_cast<char*>(::malloc(len)) : nullptr;
    for (int i = 1; i < nptrs; ++i)  // 跳过第 0 帧，即当前函数
    {
      if (demangle) {
        // https://panthema.net/2008/0901-stacktrace-demangled/
        // bin/exception_test(_ZN3Bar4testEv+0x79) [0x401909]
        char* left_par = nullptr;
        char* plus = nullptr;
        for (char* p = strings[i]; *p; ++p) {
          if (*p == '(')
            left_par = p;
          else if (*p == '+')
            plus = p;
        }

        if (left_par && plus) {
          *plus = '\0';
          int status = 0;
          char* ret = abi::__cxa_demangle(left_par + 1, demangled, &len, &status);
          *plus = '+';
          if (status == 0) {
            demangled = ret;  // ret 可能由 realloc() 重新分配
            stack.append(strings[i], left_par + 1);
            stack.append(demangled);
            stack.append(plus);
            stack.push_back('\n');
            continue;
          }
        }
      }
      // 解析失败时使用修饰后的符号名
      stack.append(strings[i]);
      stack.push_back('\n');
    }
    free(demangled);
    free(strings);
  }
  return stack;
}
}  // namespace TLSS::BASE::CurrentThread
