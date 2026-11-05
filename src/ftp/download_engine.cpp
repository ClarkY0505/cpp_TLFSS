#include "ftp/download_engine.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace TLSS::FileStorage {
std::shared_ptr<DownloadTask> DownloadEngine::create(TransferContext context, std::string name,
                                                     TransferTask::Notify notify) const {
  return std::shared_ptr<DownloadTask>(new DownloadTask(std::move(context), std::move(name), _disk,
                                                        _metadata, _objects, _stopping,
                                                        std::move(notify)));
}

DownloadTask::DownloadTask(TransferContext context, std::string name, Transfer::DiskExecutor& disk,
                           MetadataService& metadata, ObjectStorageEngine& objects,
                           const std::atomic<bool>& stopping, Notify notify)
    : TransferTask(std::move(context), stopping, std::move(notify),
                   TransferPhase::preparing_download)
    , _name(std::move(name))
    , _disk(disk)
    , _metadata(metadata)
    , _objects(objects) {}

DownloadTask::~DownloadTask() {
  cancel();
}

void DownloadTask::start() {
  auto conn = current_connection();
  if (!conn || _started)
    return;
  _started = true;
  metrics_start(false);
  auto* metadata = &_metadata;
  auto* objects = &_objects;
  auto factory = [metadata, objects, username = _context.username, directory = _context.directory,
                  name = _name]() -> Transfer::AsyncFileSource::Opened {
    auto result = metadata->find_download_hash(username, directory, name);
    if (!result.success)
      throw std::runtime_error("file not found or unavailable");
    return {objects->open_read(result.result), std::move(result.result)};
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
