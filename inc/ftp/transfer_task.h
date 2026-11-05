#ifndef TLSS_FTP_TRANSFER_TASK_H
#define TLSS_FTP_TRANSFER_TASK_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "common/net/tcp_connection.h"

namespace TLSS::FileStorage {
struct ConnectionSession;

enum class TransferPhase {
  idle,
  preparing_upload,
  receiving_upload,
  finalizing_upload,
  preparing_download,
  sending_download,
  waiting_download_ack
};

// Captured on the connection's loop. Workers use only the immutable value fields.
struct TransferContext {
  std::weak_ptr<NET::TcpConnection> connection;
  std::weak_ptr<ConnectionSession> session;
  std::uint64_t generation = 0;
  std::uint64_t user_id = 0;
  std::string username;
  std::string directory;

  using Current = std::pair<NET::TcpConnectionPtr, std::shared_ptr<ConnectionSession>>;
  Current current() const;
};

struct TransferEvent {
  enum class Kind {
    upload_ready,
    upload_complete,
    download_ready,
    download_sent,
    download_complete,
    failed
  };
  Kind kind;
  std::string value;
  std::uint64_t size = 0;
  bool close_connection = false;
};

// State transitions and notifications belong to the connection's IO loop.
// Disk adapters retain their own synchronized worker/cancellation state.
class TransferTask {
 public:
  using Notify = std::function<void(TransferEvent)>;
  virtual ~TransferTask() = default;
  TransferPhase phase() const {
    return _phase;
  }
  virtual void start() = 0;
  virtual void consume(NET::Buffer& buffer);
  virtual void acknowledge();
  virtual void cancel() noexcept = 0;

 protected:
  TransferTask(TransferContext context, const std::atomic<bool>& stopping, Notify notify,
               TransferPhase phase)
      : _context(std::move(context))
      , _stopping(stopping)
      , _notify(std::move(notify))
      , _phase(phase) {}

  NET::TcpConnectionPtr current_connection() const;
  void emit(TransferEvent event);

  TransferContext _context;
  const std::atomic<bool>& _stopping;
  Notify _notify;
  TransferPhase _phase;
  bool _cancelled = false;
};
}  // namespace TLSS::FileStorage
#endif
