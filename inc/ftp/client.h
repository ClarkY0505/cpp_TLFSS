#ifndef __INC_FTP_CLIENT_H__
#define __INC_FTP_CLIENT_H__

#include "common/net/channel.h"
#include "common/net/tcp_client.h"

#include <signal.h>
#include <memory>
#include <string>

namespace TLSS::FileStorage {
class Client {
 public:
  Client(NET::EventLoop* loop, const NET::InetAddress& server_addr);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void start();

 private:
  void on_connection(const NET::TcpConnectionPtr& conn);
  void on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buffer, TIME::Timestamp);
  void on_input();
  void on_signal();
  void on_timeout();
  void handle_command(std::string command);
  void request_exit();
  void finish_exit();

  NET::EventLoop* _loop;
  NET::TcpClient _client;
  std::unique_ptr<NET::Channel> _input_channel;
  std::unique_ptr<NET::Channel> _signal_channel;
  std::unique_ptr<NET::Channel> _timer_channel;
  int _signal_fd{-1};
  int _timer_fd{-1};
  sigset_t _old_mask{};
  bool _mask_saved{false};
  bool _started{false};
  bool _ever_connected{false};
  bool _stopping{false};
  bool _finishing{false};
  bool _drop_line{false};
  std::string _input;
};

}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_CLIENT_H__
