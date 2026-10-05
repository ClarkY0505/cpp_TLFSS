#ifndef __INC_COMMON_TCP_SERVER_H__
#define __INC_COMMON_TCP_SERVER_H__

#include "common/NoCopy.h"
#include "common/net/acceptor.h"
#include "common/net/callbacks.h"
#include "common/net/event_loop.h"
#include "common/net/event_loop_thread_pool.h"
#include "common/net/inet_address.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
namespace TLSS::NET {
class TcpServer : NoCopy {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  enum Option {
    k_no_reuse_port,
    k_ruese_port,
  };

  TcpServer(EventLoop* loop, const InetAddress& listen_addr, Option option = k_no_reuse_port);
  ~TcpServer();

  //
  // 设置底层subloop的个数
  void set_thread_num(int num);
  void set_thread_init_cb(const ThreadInitCallback& cb) {
    _thread_init_cb = cb;
  }
  void set_connection_cb(const ConnectionCb& cb) {
    _conn_cb = cb;
  }
  void set_message_cb(const MessageCb& cb) {
    _mess_cb = cb;
  }
  void set_write_complete_cb(const WriteCompleteCb& cb) {
    _write_cb = cb;
  }

  void start();

 private:
  using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;
  void new_connection(int sockfd, const InetAddress& peer_addr);
  void remove_connection(const TcpConnectionPtr& conn);
  void remove_connection_in_loop(const TcpConnectionPtr& conn);
  //
  // baseloop
  EventLoop* _loop;
  const std::string _ip_port;
  const std::string _name;
  std::unique_ptr<Acceptor> _acceptor;
  //
  // one loop per thread的核心线程池
  // 它与业务线程池的生产者消费者模型完全不一样
  // 它主要区别是mainloop唤醒subloop然后获取channel
  // 其实在这个阶段获取channel可以增加一个生产者消费者模型
  // 可能会有更好的效果，后续尝试增加一下
  // TODO
  std::shared_ptr<EventLoopThreadPool> _thread_pool;

  // 有新连接时的回调
  ConnectionCb _conn_cb;
  // 有读写消息时的回调
  MessageCb _mess_cb;
  // 消息发送完成以后的回调
  WriteCompleteCb _write_cb;
  // loop线程初始化的回
  ThreadInitCallback _thread_init_cb;

  std::atomic<int> _started;
  int _next_conn_id;
  // 保存所有的连接
  ConnectionMap _connections;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_TCP_SERVER_H__
