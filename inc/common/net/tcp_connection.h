#ifndef __INC_COMMON_NET_TCP_CONNECTION_H__
#define __INC_COMMON_NET_TCP_CONNECTION_H__

#include "common/NoCopy.h"
#include "common/net/buffer.h"
#include "common/net/callbacks.h"
#include "common/net/inet_address.h"
#include "common/net/timestamp.h"

#include <netinet/in.h>
#include <atomic>
#include <cstddef>
#include <memory>
namespace TLSS::NET {
class Channel;
class EventLoop;
class Socket;

/*
 * TcpServer  -> Acceptor => 有一个新用户连接，通过accept函数拿到connfd =>
 * TcpConnection 设置回调 => Channel => Poller => 进行Channel 回调操作（handle_event）
 * */
class TcpConnection : NoCopy, public std::enable_shared_from_this<TcpConnection> {
 public:
  TcpConnection(EventLoop* loop, const std::string& name, int sockfd, const InetAddress& local_addr,
                const InetAddress& peer_addr);
  ~TcpConnection();

  EventLoop* get_loop() const {
    return _loop;
  }
  const std::string& name() const {
    return _name;
  }
  const InetAddress& local_addr() const {
    return _local_addr;
  }
  const InetAddress& peer_addr() const {
    return _peer_addr;
  }
  bool connected() const {
    return _state.load() == k_connected;
  }
  bool disconnected() const {
    return _state.load() == k_disconnected;
  }

  /* void send(const void* message, int len); */
  /* void send(Buffer* buf); */
  void send(const std::string& buf);
  void shutdow();

  void set_connection_cb(const ConnectionCb& cb) {
    _conn_cb = cb;
  }
  void set_message_cb(const MessageCb& cb) {
    _mess_cb = cb;
  }
  void set_write_complete_cb(const WriteCompleteCb& cb) {
    _write_cb = cb;
  }
  void set_close_cb(const CloseCb& cb) {
    _close_cb = cb;
  }
  void set_high_water_mark_cb(const HighWaterMarkCb& cb) {
    _high_water_cb = cb;
  }

  // 连接建立
  void connect_established();
  // 连接销毁
  void connect_destroyed();

 private:
  enum StateE { k_disconnected, k_connecting, k_connected, k_disconnecting };
  void set_state(StateE s) {
    _state = s;
  }

  void handle_read(TIME::Timestamp receive_time);
  void handle_write();
  void handle_close();
  void handle_error();
  std::string state_to_string() const;

  void send_in_loop(const void* data, size_t len);
  void shutdown_in_loop();
  // 这里的loop不是 baseloop
  // TcpConnection在subloop中管理
  EventLoop* _loop;
  const std::string _name;
  std::atomic<StateE> _state;
  bool _reading;

  std::unique_ptr<Socket> _socket;
  std::unique_ptr<Channel> _channel;

  const InetAddress _local_addr;
  const InetAddress _peer_addr;

  ConnectionCb _conn_cb;
  MessageCb _mess_cb;
  WriteCompleteCb _write_cb;
  CloseCb _close_cb;

  HighWaterMarkCb _high_water_cb;
  size_t _high_water_mark;

  Buffer _input_buffer;
  Buffer _output_buffer;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_TCO_CONNECTION_H__
