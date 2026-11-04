#include "ftp/upload_engine.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "common/net/event_loop.h"
#include "ftp/service_logger.h"

namespace TLSS::FileStorage {
std::shared_ptr<UploadTask> UploadEngine::create(TransferContext context, UploadRequest request,
                                                 TransferTask::Notify notify) const {
    return std::shared_ptr<UploadTask>(new UploadTask(std::move(context), std::move(request),
                                                      _workers, _disk, _metadata, _objects,
                                                      _stopping, std::move(notify)));
}

UploadTask::UploadTask(TransferContext context, UploadRequest request, BASE::ThreadPool& workers,
                       Transfer::DiskExecutor& disk, MetadataService& metadata,
                       ObjectStorageEngine& objects, const std::atomic<bool>& stopping,
                       Notify notify)
    : TransferTask(std::move(context), stopping, std::move(notify),
                   TransferPhase::preparing_upload),
      _request(std::move(request)),
      _workers(workers),
      _disk(disk),
      _metadata(metadata),
      _objects(objects) {}

UploadTask::~UploadTask() { cancel(); }

void UploadTask::start() {
    auto conn = current_connection();
    if (!conn || _started) return;
    _started = true;
    const auto weak = weak_from_this();
    auto* loop = conn->get_loop();
    auto* metadata = &_metadata;
    const auto* stopping = &_stopping;
    const auto username = _context.username;
    const auto directory = _context.directory;
    const auto name = _request.name;
    const bool accepted = _workers.try_run([weak, loop, metadata, stopping, username, directory,
                                            name] {
        StorageContext<std::string> parent;
        std::string error;
        try {
            if (stopping->load())
                error = "server is stopping";
            else
                parent = metadata->prepare_upload(username, directory, name);
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "unexpected transfer failure";
        }
        loop->queue_in_loop([weak, parent = std::move(parent), error = std::move(error)]() mutable {
            if (auto task = weak.lock()) task->on_prepared(std::move(parent), std::move(error));
        });
    });
    if (!accepted) on_failed("server is busy");
}

void UploadTask::on_prepared(StorageContext<std::string> parent, std::string error) {
    auto conn = current_connection();
    if (!conn || _phase != TransferPhase::preparing_upload) return;
    if (!error.empty() || !parent.success) {
        on_failed(error.empty() ? "directory unavailable or filename already exists"
                                : std::move(error));
        return;
    }
    _parent_id = std::move(parent.result);
    const auto weak = weak_from_this();
    auto* objects = &_objects;
    auto* metadata = &_metadata;
    auto factory = [objects, size = _request.size] { return objects->create_temporary(size); };
    // Preserve the existing disk-worker execution and publish-before-register ordering.
    // Only the upload coordinator knows both storage and metadata services.
    auto finalizer = [objects, metadata, username = _context.username,
                      directory = _context.directory, name = _request.name,
                      parent_id = _parent_id](Transfer::Receiver& receiver) {
        auto object = objects->publish(receiver);
        if (!metadata->commit_upload(username, parent_id, directory, name, object.hash)) {
            throw std::runtime_error("file stored but metadata registration failed");
        }
        return object.hash;
    };
    try {
        _writer = Transfer::AsyncReceiver::create(
            _disk, conn, _request.size, std::move(factory), std::move(finalizer),
            [weak] {
                if (auto task = weak.lock()) task->on_ready();
            },
            [weak](std::string hash) {
                if (auto task = weak.lock()) task->on_done(std::move(hash));
            },
            [weak](std::string reason) {
                if (auto task = weak.lock()) task->on_failed(std::move(reason));
            });
    } catch (const std::exception& exception) {
        on_failed(exception.what());
    }
}

void UploadTask::on_ready() {
    if (!current_connection() || _phase != TransferPhase::preparing_upload) return;
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
    if (!current_connection() || _phase != TransferPhase::finalizing_upload) return;
    _phase = TransferPhase::idle;
    emit({TransferEvent::Kind::upload_complete, std::move(hash)});
}

void UploadTask::on_failed(std::string error) {
    if (!current_connection()) return;
    const bool close = _phase == TransferPhase::receiving_upload;
    if (close) service_logger()->error("upload disk write failed: {}", error);
    emit({TransferEvent::Kind::failed, std::move(error), 0, close});
}

void UploadTask::cancel() noexcept {
    _cancelled = true;
    if (_writer) _writer->cancel();
}
}  // namespace TLSS::FileStorage
