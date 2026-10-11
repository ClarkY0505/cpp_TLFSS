/* #include "common/base/mysql_connection_pool.h" */
/* #include "common/base/tlss_thread_pool.h" */
#include "ftp/server.h"
#include "ftp/service_logger.h"

#include <functional>
namespace TLSS::FileStorage {
StorageService::StorageService(NET::EventLoop* loop, const NET::InetAddress& addr,
                               const std::string& name, int subloop_size,
                               int thread_pool_threads_num, int thrad_pool_queue_max_size)
    : _thread_pool_threads_num(thread_pool_threads_num)
    , _loop(loop)
    , _tcp_server(_loop, addr, name)
    , _db_conn_pool(BASE::DbConnectionPool::get_connection_pool())
    , _th_pool("tlss_worker") {
  _tcp_server.set_connection_cb(
      std::bind(&StorageService::on_connection, this, std::placeholders::_1));
  _tcp_server.set_message_cb(std::bind(&StorageService::on_message, this, std::placeholders::_1,
                                       std::placeholders::_2, std::placeholders::_3));
  _tcp_server.set_thread_num(subloop_size);
  _th_pool.set_max_queue_size(thrad_pool_queue_max_size);
}

StorageService::~StorageService() {
  stop();
}

void StorageService::start() {
  _tcp_server.start();
  _th_pool.start(_thread_pool_threads_num);
}

void StorageService::stop() {
  _th_pool.stop();
  _db_conn_pool->shutdown();
}

void StorageService::on_connection(const NET::TcpConnectionPtr& conn) {
  if (!conn->connected()) {
    service_logger()->info("conn down : {}", conn->peer_addr().to_ip_port());
    return;
  }

  conn->set_high_water_mark(256ULL * 1024);

  conn->set_high_water_mark_cb([](const NET::TcpConnectionPtr& c, std::size_t) {
    c->stop_read();
  });

  conn->set_write_complete_cb([](const NET::TcpConnectionPtr& c) {
    c->start_read();
  });

  service_logger()->info("conn up : {}", conn->peer_addr().to_ip_port());
  conn->send("tlss>");
}

void StorageService::on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buf,
                                TIME::Timestamp timestamp) {
  (void)timestamp;

  constexpr std::size_t k_max_command_bytes = 4096;
  constexpr char k_crlf[] = "\r\n";

  while (conn->connected()) {
    const char* begin = buf->peek();
    const char* end = begin + buf->readable_bytes();
    const char* delimiter = std::search(begin, end, k_crlf, k_crlf + 2);

    if (delimiter == end) {
      // 允许末尾的 '\r' 等待下次读取补上 '\n'。
      if (buf->readable_bytes() > k_max_command_bytes + 1) {
        buf->retrieve_all();
        conn->force_close();
      }
      return;
    }

    const std::size_t length = static_cast<std::size_t>(delimiter - begin);
    if (length > k_max_command_bytes) {
      buf->retrieve_all();
      conn->force_close();
      return;
    }

    std::string command(begin, length);
    buf->retrieve(length + 2);

    if (command == "q") {
      conn->send("bye!\r\n");
      conn->shutdow();
      return;
    }

    // TODO 接入命令分配器后 将command改为 res
    conn->send(command + "\r\ntlss>");
  }
}

}  // namespace TLSS::FileStorage
