#include <array>
#include <chrono>
#include <ctime>
#include <iostream>
#include <system_error>
#include <type_traits>
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"
#include "common/base/tlss_thread_pool.h"
using namespace TLSS::BASE;
using namespace std;
static_assert(!std::is_copy_constructible_v<DbConnectionHandle>);
static_assert(std::is_move_constructible_v<DbConnectionHandle>);

void test() {
  /* DbConnectionPool* cp = DbConnectionPool::get_connection_pool(); */
  std::clock_t begin = clock();
  for (int i = 0; i < 5000; ++i) {
    DbConnection conn;
    std::string sql("select * from virtual_files");
    conn.connect("127.0.0.1", 3306, "root", "123456", "tlss_db");
    MYSQL_RES* result = conn.query(sql);
    if (result) {
      mysql_free_result(result);
    }
    /* auto connection = cp->get_connection(); */
    /* if (!connection) { */
    /* std::cerr << "unable to borrow a database connection" << std::endl; */
    /* return 1; */
    /* } */
    /* connection->query(sql); */
  }
  std::clock_t end = clock();

  std::cout << (end - begin) << std::endl;
  /* cp->shutdown(); */
}

struct QueryStats {
  int succeeded = 0;
  int borrow_failed = 0;
  int query_failed = 0;
};

QueryStats test1(int query_count) {
  DbConnectionPool* cp = DbConnectionPool::get_connection_pool();
  QueryStats stats;
  const std::string sql("select * from virtual_files");
  for (int i = 0; i < query_count; ++i) {
    auto connection = cp->get_connection();
    if (!connection) {
      ++stats.borrow_failed;
      continue;
    }
    MYSQL_RES* result = connection->query(sql);
    if (!result) {
      ++stats.query_failed;
      continue;
    }
    mysql_free_result(result);
    ++stats.succeeded;
  }
  return stats;
}

bool benchmark(int thread_count) {
  constexpr int total_queries = 5000;
  std::array<QueryStats, 4> results{};
  ThreadPool pool("MysqlQueryBenchmark");
  const auto begin = std::chrono::steady_clock::now();
  pool.start(thread_count);
  for (int i = 0; i < thread_count; ++i) {
    if (!pool.run([&, i] {
          results[i] = test1(total_queries / thread_count);
        })) {
      std::cerr << "提交 test1 失败\n";
      pool.stop();
      return false;
    }
  }
  pool.stop();
  const auto elapsed = std::chrono::steady_clock::now() - begin;

  QueryStats total;
  for (int i = 0; i < thread_count; ++i) {
    total.succeeded += results[i].succeeded;
    total.borrow_failed += results[i].borrow_failed;
    total.query_failed += results[i].query_failed;
  }
  const auto milliseconds = std::chrono::duration<double, std::milli>(elapsed).count();
  std::cout << thread_count << " threads: " << milliseconds << " ms, "
            << "succeeded=" << total.succeeded << ", borrow_failed=" << total.borrow_failed
            << ", query_failed=" << total.query_failed << '\n';
  return total.succeeded == total_queries;
}

int main() {
  DbConnectionPool::get_connection_pool();  // 两组测试共用已初始化的连接池。
  const bool single_ok = benchmark(1);
  const bool parallel_ok = benchmark(4);
  return single_ok && parallel_ok ? 0 : 1;
}
