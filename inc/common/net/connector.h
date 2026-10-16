#ifndef __INC_COMMON_NET_CONNECTOR_H__
#define __INC_COMMON_NET_CONNECTOR_H__

#include <functional>
#include <memory>
#include <utility>
#include "common/NoCopy.h"
#include "common/net/inet_address.h"
namespace TLSS::NET {
class Channel;
class EventLoop;

class Connector : NoCopy, public std::enable_shared_from_this<Connector> {
 public:
  using NewConnectionCb = std::function<void(int sockfd)>;
  Connector(EventLoop* loop, const InetAddress& server_addr);
  ~Connector();

  // 以下命令允许跨线程调用，并统一按入队顺序在 loop 执行。
  // stop() 返回仅表示已提交请求，不等待正在执行的连接回调结束。
  // 调用期间须持有 Connector；loop 必须存活并处理完清理任务后再退出。
  void start();
  void restart();
  void stop();

  // 替换也入同一队列：先设置、后 start，即可保证首次连接使用新回调。
  void set_new_connection_cb(NewConnectionCb cb);

  const InetAddress& server_address() const {
    return _server_addr;
  }

 private:
  enum States { k_disconnected, k_connecting, k_connected };
  static constexpr int k_max_retry_delay_ms = 30 * 1000;
  static constexpr int k_init_retry_delay_ms = 500;

  void set_state(States s) {
    _state = s;
  }
  void start_in_loop();
  void stop_in_loop();
  void connect();
  void connecting(int sockfd);
  void handle_write();
  void handle_error();
  void retry(int sockfd);
  void handle_retry();
  void cancel_retry();
  int remove_and_reset_channel();

  EventLoop* _loop;
  InetAddress _server_addr;
  // 所有可变连接状态（包括回调、Channel、重试定时器）只在 loop 线程访问。
  // 安全性来自线程归属，而不是给个别标志加 atomic。
  bool _connect;
  States _state;
  std::unique_ptr<Channel> _channel;
  NewConnectionCb _new_connection_cb;
  int _retry_delay_ms;
  int _retry_fd{-1};
  bool _retry_pending{false};
  std::unique_ptr<Channel> _retry_channel;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_CONNECTOR_H__
