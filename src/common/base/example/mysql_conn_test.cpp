#include <ctime>
#include <iostream>
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"
using namespace TLSS::BASE;

int main() {
  DbConnectionPool* cp = DbConnectionPool::get_connection_pool();
  std::clock_t begin = clock();
  for (int i = 0; i < 1000; ++i) {
    /* DbConnection conn; */
    std::string sql("select * from virtual_files");
    /* conn.connect("127.0.0.1",3306,"root","123456","tlss_db"); */
    /* MYSQL_RES* result = conn.query(sql); */
    auto sp = cp->get_connection();
    sp->query(sql);
  }
  std::clock_t end = clock();

  std::cout << (end - begin) << std::endl;
}
