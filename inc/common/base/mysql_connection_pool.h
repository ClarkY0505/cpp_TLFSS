#ifndef __INC_COMMON_BASE_MYSQL_CONNECTION_POOL_H__
#define __INC_COMMON_BASE_MYSQL_CONNECTION_POOL_H__
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include "common/base/tlss_thread.h"

namespace TLSS::BASE {
class DbConnection;
class DbConnectionPool {
 public:
  static DbConnectionPool* get_connection_pool();
  //
  // 提供接口，从连接池中获取一个可用空闲连接
  std::shared_ptr<DbConnection> get_connection();

 private:
  DbConnectionPool();
  ~DbConnectionPool();

  bool load_config_file();
  bool set_variable(const std::string& key, const std::string& value);

  void produce_connection_task();
  void scanner_connection();
  void stop_background_threads();

  std::string _ip;
  unsigned short _port;
  std::string _username;
  std::string _password;
  int _init_size;
  int _max_size;
  int _max_idle_time;  // 连接池最大空闲时间
  int _connection_timeout;

  Thread _produce_thread;
  Thread _scanner_thread;

  std::queue<std::unique_ptr<DbConnection>> _connection_queue;
  std::mutex _mutex;
  std::condition_variable _cond;
  std::atomic<int> _connection_cnt;
  bool _stopping;
};
}  // namespace TLSS::BASE
#endif  // __INC_COMMON_BASE_MYSQL_CONNECTION_POOL_H__
