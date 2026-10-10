#include <atomic>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <future>
#include <semaphore.h>
#include <thread>

#include "common/base/tlss_thread.h"

namespace {
std::atomic<bool> release_after_post{false};
}  // namespace

// The current Thread entry posts its startup semaphore immediately before
// invoking its callback. Hold it there until the Thread wrapper is destroyed.
extern "C" int sem_post(sem_t* sem) {
  using SemPost = int (*)(sem_t*);
  static auto real_post = reinterpret_cast<SemPost>(dlsym(RTLD_NEXT, "sem_post"));
  const int result = real_post(sem);
  while (!release_after_post.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  return result;
}

int main() {
  std::promise<void> completed;
  auto future = completed.get_future();
  {
    TLSS::BASE::Thread worker([&completed] { completed.set_value(); }, "DetachedLifetime");
    worker.start();
  }

  release_after_post.store(true, std::memory_order_release);
  if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    std::fputs("detached callback did not complete\n", stderr);
    return 1;
  }
  std::puts("detached callback completed after Thread destruction");
}
