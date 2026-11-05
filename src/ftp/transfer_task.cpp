#include "ftp/transfer_task.h"

#include <cassert>
#include <stdexcept>

#include "common/net/event_loop.h"
#include "ftp/context.h"

namespace TLSS::FileStorage {
TransferContext::Current TransferContext::current() const {
  auto conn = connection.lock();
  auto original = session.lock();
  if (!conn || !original || !conn->connected()) {
    return {};
  }
  assert(conn->get_loop()->is_in_loop_thread());
  if (original->transfer_generation != generation ||
      original->login_state != LoginState::k_authenticated || original->user_id != user_id ||
      original->name != username || original->current_directory != directory) {
    return {};
  }
  return {std::move(conn), std::move(original)};
}

NET::TcpConnectionPtr TransferTask::current_connection() const {
  if (_cancelled) {
    return {};
  }
  auto current = _context.current();
  if (!current.first) {
    return {};
  }
  if (_stopping.load()) {
    current.first->force_close();
    return {};
  }
  return std::move(current.first);
}

void TransferTask::emit(TransferEvent event) {
  if (current_connection() && _notify) {
    _notify(std::move(event));
  }
}

void TransferTask::metrics_start(bool upload) noexcept {
  if (!_context.metrics || _metric_active)
    return;
  _metric_active = true;
  _metric_upload = upload;
  _context.metrics->add(upload ? OBSERVABILITY::Metric::upload_started
                               : OBSERVABILITY::Metric::download_started);
  _context.metrics->add(upload ? OBSERVABILITY::Metric::upload_active
                               : OBSERVABILITY::Metric::download_active);
}

void TransferTask::metrics_finish(OBSERVABILITY::Metric result) noexcept {
  if (!_metric_active)
    return;
  _metric_active = false;
  _context.metrics->subtract(_metric_upload ? OBSERVABILITY::Metric::upload_active
                                            : OBSERVABILITY::Metric::download_active);
  // Upload commits are counted in the disk finalizer, even if the task no longer exists.
  if (result != OBSERVABILITY::Metric::upload_committed)
    _context.metrics->add(result);
}

void TransferTask::consume(NET::Buffer&) {
  throw std::logic_error("transfer is not receiving a file");
}

void TransferTask::acknowledge() {
  throw std::logic_error("transfer is not waiting for an acknowledgement");
}
}  // namespace TLSS::FileStorage
