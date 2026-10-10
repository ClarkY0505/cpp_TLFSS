#include <atomic>
#include <cerrno>
#include <cstdio>
#include <dlfcn.h>
#include <pthread.h>
#include <system_error>

#include "common/base/tlss_thread_pool.h"

namespace {
std::atomic<int> create_count{0};
std::atomic<int> fail_on_create{0};

bool check_failed_start(int fail_at) {
  create_count = 0;
  fail_on_create = fail_at;

  TLSS::BASE::ThreadPool pool("StartFailureTest");
  bool caught = false;
  try {
    pool.start(4);
  } catch (const std::system_error& error) {
    caught = error.code().value() == EAGAIN;
  }

  fail_on_create = 0;
  if (!caught) {
    std::fprintf(stderr, "expected thread creation %d to fail\n", fail_at);
    return false;
  }

  pool.stop();
  pool.stop();
  if (pool.run([] {})) {
    std::fprintf(stderr, "pool accepted a task after failed start\n");
    return false;
  }
  return true;
}
}  // namespace

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                              void* (*start_routine)(void*), void* arg) {
  using CreateFunction = decltype(&pthread_create);
  static auto real_create = reinterpret_cast<CreateFunction>(dlsym(RTLD_NEXT, "pthread_create"));
  if (++create_count == fail_on_create.load()) {
    return EAGAIN;
  }
  return real_create(thread, attr, start_routine, arg);
}

int main() {
  if (!check_failed_start(1) || !check_failed_start(3)) {
    return 1;
  }
  std::puts("thread pool startup rollback passed");
}
