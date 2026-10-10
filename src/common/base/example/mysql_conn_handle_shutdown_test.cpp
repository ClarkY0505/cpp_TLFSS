#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "common/base/mysql_connection_pool.h"

namespace {
std::atomic<bool> holder_reached_release{false};

void verify_pool_waited_for_handle() {
  if (!holder_reached_release.load(std::memory_order_acquire)) {
    std::fputs("pool exited before the borrowed connection was released\n", stderr);
    std::_Exit(1);
  }
  std::puts("pool waited for the borrowed connection");
}
}  // namespace

int main() {
  // Register first so this check runs after the singleton pool destructor.
  if (std::atexit(verify_pool_waited_for_handle) != 0) {
    return 2;
  }

  auto* pool = TLSS::BASE::DbConnectionPool::get_connection_pool();
  auto connection = pool->get_connection();
  if (!connection) {
    return 3;
  }

  std::thread([connection = std::move(connection)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    holder_reached_release.store(true, std::memory_order_release);
    connection.reset();
  }).detach();
}
