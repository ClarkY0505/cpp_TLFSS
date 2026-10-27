#ifndef __INC_FTP_ASYNC_FILE_SOURCE_H__
#define __INC_FTP_ASYNC_FILE_SOURCE_H__

#include "ftp/async_file_receiver.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

namespace TLSS::FileStorage::Transfer {

// File open/read/close run on DiskExecutor; public state belongs to the connection's loop.
class AsyncFileSource : public std::enable_shared_from_this<AsyncFileSource> {
 public:
  struct Info {
    std::uint64_t size = 0;
    std::string tag;
  };
  struct Opened {
    std::shared_ptr<SourceFile> file;
    std::string tag;
  };
  using Factory = std::function<Opened()>;
  using Ready = std::function<void(const Info&)>;
  using Failed = std::function<void(std::string)>;

  static std::shared_ptr<AsyncFileSource> open(DiskExecutor& executor,
                                               const NET::TcpConnectionPtr& conn,
                                               Factory factory, Ready ready, Failed failed);
  ~AsyncFileSource();
  AsyncFileSource(const AsyncFileSource&) = delete;
  AsyncFileSource& operator=(const AsyncFileSource&) = delete;

  std::uint64_t size() const;
  const std::string& tag() const;
  NET::TcpConnection::FileReadFunction reader();
  void cancel() noexcept;

 private:
  using Completion = NET::TcpConnection::FileReadCompletion;
  struct State : public std::enable_shared_from_this<State> {
    State(DiskExecutor& executor_arg, NET::EventLoop* loop_arg, Factory factory_arg);
    ~State();
    bool is_cancelled();
    bool launch_initialize();
    void run_initialize();
    void request(off_t offset, std::size_t count, Completion callback);
    void run_read(off_t offset, std::size_t count);
    // mutex must be held. Cancellation must deliver an error, not discard a pending read.
    void post_read_locked(Completion callback, std::error_code error, std::string data);
    void cancel() noexcept;
    static std::error_code make_error(int value);

    DiskExecutor& executor;
    NET::EventLoop* loop;
    std::uint64_t token = 0;
    Factory factory;
    std::weak_ptr<AsyncFileSource> owner;
    // Accessed and released only by workers.
    std::shared_ptr<SourceFile> file;
    std::mutex mutex;
    Completion completion;
    std::uint64_t file_size = 0;
    bool initialized = false;
    bool busy = false;
    bool cancelled = false;
    bool terminal = false;
    bool post_enabled = true;
  };

  AsyncFileSource(const NET::TcpConnectionPtr& conn, std::shared_ptr<State> state,
                  Ready ready, Failed failed);
  void on_opened(Info info, std::string error);
  void report_error(const NET::TcpConnectionPtr& conn, std::string error);

  std::weak_ptr<NET::TcpConnection> _connection;
  std::shared_ptr<State> _state;
  Info _info;
  Ready _ready;
  Failed _failed;
  bool _opened = false;
};

}  // namespace TLSS::FileStorage::Transfer
#endif  // __INC_FTP_ASYNC_FILE_SOURCE_H__
