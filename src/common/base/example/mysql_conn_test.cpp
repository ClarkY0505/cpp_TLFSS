#include <ctime>
#include <iostream>
#include <type_traits>
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"
using namespace TLSS::BASE;

static_assert(!std::is_copy_constructible_v<DbConnectionHandle>);
static_assert(std::is_move_constructible_v<DbConnectionHandle>);

int main() {
  DbConnectionPool* cp = DbConnectionPool::get_connection_pool();
  std::clock_t begin = clock();
  for (int i = 0; i < 1000; ++i) {
    /* DbConnection conn; */
    std::string sql("select * from virtual_files");
    /* conn.connect("127.0.0.1",3306,"root","123456","tlss_db"); */
    /* MYSQL_RES* result = conn.query(sql); */
    auto connection = cp->get_connection();
    if (!connection) {
      std::cerr << "unable to borrow a database connection" << std::endl;
      return 1;
    }
    connection->query(sql);
  }
  std::clock_t end = clock();

  std::cout << (end - begin) << std::endl;
  cp->shutdown();
}
