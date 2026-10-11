#ifndef __INC_FTP_SERVER_H__
#define __INC_FTP_SERVER_H__

#include "common/base/mysql_connection_pool.h"
#include "common/base/tlss_thread_pool.h"
#include "common/net/tcp_server.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
namespace TLSS::FileStorage {
class StorageService {
 public:
  explicit StorageService(NET::EventLoop* loop, const NET::InetAddress& addr,
                          const std::string& name = "tlss_service", int subloop_size = 4,
                          int thread_pool_threads_num = 8, int thrad_pool_queue_max_size = 256);
  ~StorageService();
  void start();
  // 从主 EventLoop 线程以外调用；先排空任务和连接，再释放资源。
  // 重复调用等待同一次停机完成；返回数据库连接池是否在期限内关闭。
  bool stop(std::chrono::milliseconds grace_period = std::chrono::seconds(5));

 private:
  void on_connection(const NET::TcpConnectionPtr& conn);
  void on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buf, TIME::Timestamp);

  void handle_file_query();
  void handle_upload();
  void handle_download();

  int _thread_pool_threads_num;
  NET::EventLoop* _loop;
  NET::TcpServer _tcp_server;
  BASE::DbConnectionPool* _db_conn_pool;
  BASE::ThreadPool _th_pool;
  std::atomic<bool> _stopping{false};
  std::mutex _stop_mutex;
  std::condition_variable _stop_done;
  bool _started = false;
  bool _stop_started = false;
  bool _stopped = false;
  bool _stop_succeeded = false;
};
}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_SERVER_H__
