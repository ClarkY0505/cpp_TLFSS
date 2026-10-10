#include <chrono>
#include <cstdio>

#include "common/base/mysql_connection_pool.h"

int main() {
  using namespace std::chrono_literals;
  using TLSS::BASE::DbConnectionPool;

  auto* pool = DbConnectionPool::get_connection_pool();
  auto connection = pool->get_connection();
  if (!connection) {
    std::fputs("could not borrow connection\n", stderr);
    return 1;
  }

  // 借出的句柄仍被持有，短期限关闭应报告失败，而不能销毁池。
  if (pool->shutdown(30ms)) {
    std::fputs("shutdown ignored outstanding handle\n", stderr);
    return 2;
  }

  // 关闭中归还连接后再次关闭应成功，析构时再次调用也不应重复 join。
  connection.reset();
  if (!pool->shutdown(500ms)) {
    std::fputs("shutdown did not finish after return\n", stderr);
    return 3;
  }
  if (!pool->shutdown(500ms)) {
    std::fputs("repeated shutdown failed\n", stderr);
    return 4;
  }
  if (pool->get_connection()) {
    std::fputs("closed pool issued a connection\n", stderr);
    return 5;
  }
  std::puts("lifecycle checks passed");
}
