#ifndef __INC_COMMON_BASE_MYSQL_CONNECTION_H__
#define __INC_COMMON_BASE_MYSQL_CONNECTION_H__

#include "mysql/mysql.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>
namespace TLSS::BASE {
using DbValue = std::optional<std::string>;
using DbRow = std::vector<DbValue>;

struct DbQueryResult {
  bool success = false;
  std::vector<DbRow> rows;
};

class DbConnection {
 public:
  // 初始化数据库连接
  DbConnection();
  ~DbConnection();
  // 在建立连接前设置 MySQL 网络超时；三个可选参数的单位均为秒。
  // 未指定时使用 5 秒默认值，连接池可按配置显式传入其他值。
  bool connect(const std::string& ip, unsigned short port, const std::string& username,
               const std::string& password, const std::string& dbname,
               unsigned int connect_timeout_sec = 5, unsigned int read_timeout_sec = 5,
               unsigned int write_timeout_sec = 5);
  bool ping();
  // insert delete update
  bool update(const std::string& sql);
  //  select
  MYSQL_RES* query(const std::string& sql);

  // 参数化 SELECT：没有匹配记录时，success 为 true，rows 为空。
  DbQueryResult query_prepared(const std::string& sql, const std::vector<DbValue>& params = {});

  // 参数化 INSERT、UPDATE、DELETE，以及不返回结果集的语句。
  bool update_prepared(const std::string& sql, const std::vector<DbValue>& params = {});

  // 刷新连接的起始空闲时间点
  void refresh_alive_time() {
    _alive_time = std::chrono::steady_clock::now();
  }
  // 返回存活时间
  std::chrono::milliseconds::rep get_alive_time() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 _alive_time)
        .count();
  }

 private:
  MYSQL* _conn;
  std::chrono::steady_clock::time_point _alive_time;
};
}  // namespace TLSS::BASE

#endif  // __INC_COMMON_BASE_MYSQL_CONNECTION_H__
