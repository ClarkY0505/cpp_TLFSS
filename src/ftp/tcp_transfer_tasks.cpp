#include "ftp/tcp_transfer_tasks.h"

#include "common/net/event_loop.h"
#include "ftp/service_logger.h"

#include <stdexcept>
#include <utility>

namespace TLSS::FileStorage {
std::shared_ptr<UploadTask> UploadTask::create(TransferContext context, UploadRequest request,
                                               BASE::ThreadPool& workers,
                                               Transfer::DiskExecutor& disk,
                                               StorageService& service,
                                               const std::atomic<bool>& stopping, Notify notify) {
  return std::shared_ptr<UploadTask>(new UploadTask(std::move(context), std::move(request), workers,
                                                    disk, service, stopping, std::move(notify)));
}

UploadTask::UploadTask(TransferContext context, UploadRequest request, BASE::ThreadPool& workers,
                       Transfer::DiskExecutor& disk, StorageService& service,
                       const std::atomic<bool>& stopping, Notify notify)
    : TransferTask(std::move(context), stopping, std::move(notify), TransferPhase::preparing_upload)
    , _request(std::move(request))
    , _workers(workers)
    , _disk(disk)
    , _service(service) {}

UploadTask::~UploadTask() {
  cancel();
}

void UploadTask::start() {
  auto conn = current_connection();
  if (!conn || _started) {
    return;
  }
  _started = true;
  metrics_start(true);
  const auto weak = weak_from_this();
  auto* loop = conn->get_loop();
  auto* service = &_service;
  const auto* stopping = &_stopping;
  const auto context =
      StorageRequestContext{_context.user_id, _context.username, _context.directory};
  const bool accepted =
      _workers.try_run([weak, loop, service, stopping, context, request = _request] {
        StorageContext<std::string> parent;
        std::string error;
        try {
          if (stopping->load()) {
            error = "server is stopping";
          } else {
            parent = service->prepare_upload(context, request);
          }
        } catch (const std::exception& exception) {
          error = exception.what();
        } catch (...) {
          error = "unexpected transfer failure";
        }
        loop->queue_in_loop([weak, parent = std::move(parent), error = std::move(error)]() mutable {
          if (auto task = weak.lock()) {
            task->on_prepared(std::move(parent), std::move(error));
          }
        });
      });
  if (!accepted) {
    if (_context.metrics)
      _context.metrics->add(OBSERVABILITY::Metric::commands_rejected);
    on_failed("server is busy");
  }
}

void UploadTask::on_prepared(StorageContext<std::string> parent, std::string error) {
  auto conn = current_connection();
  if (!conn || _phase != TransferPhase::preparing_upload) {
    return;
  }
  if (!error.empty() || !parent.success) {
    on_failed(error.empty() ? "directory unavailable or filename already exists"
                            : std::move(error));
    return;
  }
  const auto weak = weak_from_this();
  auto* service = &_service;
  auto factory = [service, size = _request.size] {
    return service->create_temporary_upload(size);
  };
  auto finalizer =
      [service,
       context = StorageRequestContext{_context.user_id, _context.username, _context.directory},
       name = _request.name, parent_id = std::move(parent.result)](Transfer::Receiver& receiver) {
        return service->commit_upload(context, parent_id, name, receiver);
      };
  try {
    _writer = Transfer::AsyncReceiver::create(
        _disk, conn, _request.size, std::move(factory), std::move(finalizer),
        [weak] {
          if (auto task = weak.lock()) {
            task->on_ready();
          }
        },
        [weak](std::string hash) {
          if (auto task = weak.lock()) {
            task->on_done(std::move(hash));
          }
        },
        [weak](std::string reason) {
          if (auto task = weak.lock()) {
            task->on_failed(std::move(reason));
          }
        });
  } catch (const std::exception& exception) {
    on_failed(exception.what());
  }
}

void UploadTask::on_ready() {
  if (!current_connection() || _phase != TransferPhase::preparing_upload) {
    return;
  }
  _phase = TransferPhase::receiving_upload;
  emit({TransferEvent::Kind::upload_ready, {}});
  if (_request.size == 0 && !_cancelled) {
    _phase = TransferPhase::finalizing_upload;
    _writer->finish();
  }
}

void UploadTask::consume(NET::Buffer& buffer) {
  if (_phase != TransferPhase::receiving_upload || !_writer || _cancelled) {
    throw std::runtime_error("upload receiver is missing or unavailable");
  }
  _writer->feed(buffer);
  if (_writer->remaining() == 0) {
    _phase = TransferPhase::finalizing_upload;
    _writer->finish();
  }
}

void UploadTask::on_done(std::string hash) {
  if (!current_connection() || _phase != TransferPhase::finalizing_upload) {
    return;
  }
  metrics_finish(OBSERVABILITY::Metric::upload_committed);
  _phase = TransferPhase::idle;
  emit({TransferEvent::Kind::upload_complete, std::move(hash)});
}

void UploadTask::on_failed(std::string error) {
  metrics_finish(OBSERVABILITY::Metric::upload_failed);
  if (!current_connection()) {
    return;
  }
  const bool close = _phase == TransferPhase::receiving_upload;
  if (close) {
    service_logger()->error("upload disk write failed: {}", error);
  }
  emit({TransferEvent::Kind::failed, std::move(error), 0, close});
}

void UploadTask::cancel() noexcept {
  metrics_finish(OBSERVABILITY::Metric::upload_cancelled);
  _cancelled = true;
  if (_writer) {
    _writer->cancel();
  }
}

std::shared_ptr<DownloadTask> DownloadTask::create(TransferContext context, std::string name,
                                                   Transfer::DiskExecutor& disk,
                                                   StorageService& service,
                                                   const std::atomic<bool>& stopping,
                                                   Notify notify) {
  return std::shared_ptr<DownloadTask>(new DownloadTask(std::move(context), std::move(name), disk,
                                                        service, stopping, std::move(notify)));
}

DownloadTask::DownloadTask(TransferContext context, std::string name, Transfer::DiskExecutor& disk,
                           StorageService& service, const std::atomic<bool>& stopping,
                           Notify notify)
    : TransferTask(std::move(context), stopping, std::move(notify),
                   TransferPhase::preparing_download)
    , _name(std::move(name))
    , _disk(disk)
    , _service(service) {}

DownloadTask::~DownloadTask() {
  cancel();
}

void DownloadTask::start() {
  auto conn = current_connection();
  if (!conn || _started)
    return;
  _started = true;
  metrics_start(false);
  auto factory =
      [service = &_service,
       context = StorageRequestContext{_context.user_id, _context.username, _context.directory},
       name = _name]() -> Transfer::AsyncFileSource::Opened {
    auto object = service->open_download(context, name);
    return {std::move(object.source), std::move(object.hash)};
  };
  const auto weak = weak_from_this();
  try {
    _source = Transfer::AsyncFileSource::open(
        _disk, conn, std::move(factory),
        [weak](const Transfer::AsyncFileSource::Info& info) {
          if (auto task = weak.lock())
            task->on_ready(info);
        },
        [weak](std::string error) {
          if (auto task = weak.lock())
            task->on_failed(std::move(error));
        });
  } catch (const std::exception& exception) {
    on_failed(exception.what());
  }
}

void DownloadTask::on_ready(const Transfer::AsyncFileSource::Info& info) {
  auto conn = current_connection();
  if (!conn || _phase != TransferPhase::preparing_download)
    return;
  if (!_source)
    throw std::runtime_error("download source is missing");
  _phase = TransferPhase::sending_download;
  emit({TransferEvent::Kind::download_ready, info.tag, info.size});
  const auto source = _source;
  const auto weak = weak_from_this();
  conn->send_file_async(0, info.size, source->reader(),
                        [weak, source](std::error_code error, std::uint64_t) {
                          source->cancel();
                          if (auto task = weak.lock())
                            task->on_sent(error);
                        });
}

void DownloadTask::on_sent(std::error_code error) {
  if (!current_connection())
    return;
  _source.reset();
  if (error) {
    metrics_finish(OBSERVABILITY::Metric::download_failed);
    emit({TransferEvent::Kind::failed, error.message(), 0, true});
    return;
  }
  _phase = TransferPhase::waiting_download_ack;
  emit({TransferEvent::Kind::download_sent, {}});
}

void DownloadTask::acknowledge() {
  if (!current_connection())
    return;
  if (_phase != TransferPhase::waiting_download_ack) {
    throw std::logic_error("download is not waiting for an acknowledgement");
  }
  metrics_finish(OBSERVABILITY::Metric::download_completed);
  _phase = TransferPhase::idle;
  emit({TransferEvent::Kind::download_complete, {}});
}

void DownloadTask::on_failed(std::string error) {
  metrics_finish(OBSERVABILITY::Metric::download_failed);
  emit({TransferEvent::Kind::failed, std::move(error), 0,
        _phase != TransferPhase::preparing_download});
}

void DownloadTask::cancel() noexcept {
  metrics_finish(OBSERVABILITY::Metric::download_cancelled);
  _cancelled = true;
  if (_source)
    _source->cancel();
}

}  // namespace TLSS::FileStorage
