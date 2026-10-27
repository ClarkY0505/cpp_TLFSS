#ifndef __INC_FTP_CLIENT_H__
#define __INC_FTP_CLIENT_H__

#include "common/net/channel.h"
#include "common/net/tcp_client.h"
#include "ftp/async_file_receiver.h"
#include "ftp/file_transfer.h"

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
  enum class FileTransferState {
    idle,
    waiting_upload_ready,
    waiting_upload_result,

    waiting_download_header,
    preparing_download,
    receiving_download,
    waiting_download_done,
    finalizing_download,
    waiting_download_ok
  };

  void on_connection(const NET::TcpConnectionPtr& conn);
  void on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buffer,
                  TIME::Timestamp timestamp);
  void on_input();
  void on_signal();
  void on_timeout();
  void handle_command(std::string command);
  void request_exit();
  void finish_exit();
  void upload(const std::string& args);
  void download(const std::string& args);
  void handle_transfer_response(const NET::TcpConnectionPtr& conn, const std::string& line);
  void reset_transfer();

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

  bool _authenticated = false;

  FileTransferState _transfer_state = FileTransferState::idle;

  std::shared_ptr<Transfer::SourceFile> _upload_source;
  /* std::shared_ptr<Transfer::Receiver> _download_receiver; */
  std::shared_ptr<Transfer::AsyncReceiver> _download_writer;
  Transfer::DiskExecutor _disk_executor{"tlss_cli_disk"};
  // 异步回调访问 Client 前检查这个生命周期标记。
  std::shared_ptr<int> _disk_lifetime = std::make_shared<int>(0);
  std::filesystem::path _download_destination;
  std::string _download_hash;
};

}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_CLIENT_H__
