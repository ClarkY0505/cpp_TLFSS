#ifndef __INC_COMMON_NET_TCP_CONNECTION_H__
#define __INC_COMMON_NET_TCP_CONNECTION_H__

#include "common/NoCopy.h"
#include "common/net/buffer.h"
#include "common/net/callbacks.h"
#include "common/net/inet_address.h"
#include "common/net/timestamp.h"
#include "common/net/write_types.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>
#include <unordered_set>
namespace TLSS::NET {
class Channel;
class EventLoop;
class Socket;
class WriteScheduler;

/*
 * TcpServer  -> Acceptor => 有一个新用户连接，通过accept函数拿到connfd =>
 * TcpConnection 设置回调 => Channel => Poller => 进行Channel 回调操作（handle_event）
 * */
class TcpConnection : NoCopy, public std::enable_shared_from_this<TcpConnection> {
 public:
  static constexpr std::size_t kAsyncFileChunk = detail::kAsyncFileChunk;
  using FileSendCallback = detail::FileSendCallback;
  using FileReadCompletion = detail::FileReadCompletion;
  using FileReadFunction = detail::FileReadFunction;
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

  // 可能存在buf.c_str()在执行回调前失效
  /* void send(const std::string& buf); */
  void send(std::string message);

  //
  // fd 必须对应可读的普通文件
  //
  // 内部同步复制 fd，函数返回后，调用方即可关闭原
  // 使用显式 offset，不改变原 fd 的当前文件偏移量
  //
  // 本函数返回不代表发送完成，以 callback 为准
  void send_file(int fd, off_t offset, uint64_t count, FileSendCallback cb = {});

  // reader must complete every request with exactly count bytes or an error.
  // The callback reports bytes submitted to the socket, not peer confirmation.
  void send_file_async(off_t offset, std::uint64_t count, FileReadFunction reader,
                       FileSendCallback cb = {});
  //
  // 异步落盘任务的独立的磁盘读取暂停
  // token 标识一个独立的磁盘接收任务。
  // 恢复时只解除对应任务设置的暂停。
  void pause_read_for_disk(std::uint64_t token);
  void resume_read_after_disk(std::uint64_t token);

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
  void set_high_water_mark(std::size_t bytes);
  void stop_read();
  void start_read();
  // 在所属 loop 暂停读取新请求；后续写完成回调也不会重新开启读取。
  void pause_read_for_shutdown();
  // 发送缓冲区排空后半关闭写端；继续读取并丢弃数据，以等待对端 FIN。
  void shutdown_for_server_stop();
  void force_close();

  // 连接建立
  void connect_established();
  // 连接销毁
  void connect_destroyed();

 private:
  enum StateE { k_disconnected, k_connecting, k_connected, k_disconnecting };
  void set_state(StateE s) {
    _state.store(s);
  }

  void handle_read(TIME::Timestamp receive_time);
  void handle_write();
  void handle_close();
  void handle_error();
  std::string state_to_string() const;

  void send_in_loop(std::string data);
  FileReadCompletion make_file_read_completion(std::uint64_t sequence);
  void on_write_drained();
  void on_write_high_water(std::size_t bytes);

  //
  // 异步落盘任务的独立的磁盘读取暂停
  bool should_read_in_loop() const;
  void update_read_interest();
  void schedule_buffered_input();

  // 发送缓冲区背压。
  bool _send_read_paused = false;

  // 停机期间只读取并丢弃数据，用来等待对端 FIN。
  bool _read_eof_only = false;

  // 已安排一次已有输入缓冲区的处理。
  // 处理之前暂不继续从 socket 读取，防止积压增长。
  bool _buffered_input_pending = false;

  std::unordered_set<std::uint64_t> _disk_read_pauses;
  TIME::Timestamp _last_receive_time;

  void fail_writes(int error);

  void shutdown_in_loop();
  // 这里的loop不是 baseloop
  // TcpConnection在subloop中管理
  EventLoop* _loop;
  const std::string _name;
  std::atomic<StateE> _state;
  bool _reading;
  bool _read_paused_for_shutdown;

  std::unique_ptr<Socket> _socket;
  std::unique_ptr<Channel> _channel;
  std::unique_ptr<WriteScheduler> _write_scheduler;

  const InetAddress _local_addr;
  const InetAddress _peer_addr;

  ConnectionCb _conn_cb;
  MessageCb _mess_cb;
  WriteCompleteCb _write_cb;
  CloseCb _close_cb;

  HighWaterMarkCb _high_water_cb;

  Buffer _input_buffer;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_TCO_CONNECTION_H__
