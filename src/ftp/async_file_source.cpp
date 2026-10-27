#include "ftp/async_file_source.h"

#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

namespace TLSS::FileStorage::Transfer {

std::shared_ptr<AsyncFileSource> AsyncFileSource::open(
    DiskExecutor& executor, const NET::TcpConnectionPtr& conn,
    Factory factory, Ready ready, Failed failed) {
  if (!conn || !conn->connected()) {
    throw std::runtime_error("connection is unavailable");
  }
  assert(conn->get_loop()->is_in_loop_thread());
  auto state = std::make_shared<State>(executor, conn->get_loop(), std::move(factory));
  auto source = std::shared_ptr<AsyncFileSource>(
      new AsyncFileSource(conn, state, std::move(ready), std::move(failed)));
  state->owner = source;
  executor.attach(state);
  if (!state->launch_initialize()) {
    source->cancel();
    throw std::runtime_error("disk executor is stopping or unavailable");
  }
  return source;
}

AsyncFileSource::AsyncFileSource(const NET::TcpConnectionPtr& conn,
                                 std::shared_ptr<State> state, Ready ready, Failed failed)
    : _connection(conn)
    , _state(std::move(state))
    , _ready(std::move(ready))
    , _failed(std::move(failed)) {}

AsyncFileSource::~AsyncFileSource() { cancel(); }
std::uint64_t AsyncFileSource::size() const { return _info.size; }
const std::string& AsyncFileSource::tag() const { return _info.tag; }
void AsyncFileSource::cancel() noexcept { _state->cancel(); }

NET::TcpConnection::FileReadFunction AsyncFileSource::reader() {
  assert(_state->loop->is_in_loop_thread());
  if (!_opened) throw std::logic_error("file source is not ready");
  auto self = shared_from_this();
  return [self](off_t offset, std::size_t count, Completion completion) {
    self->_state->request(offset, count, std::move(completion));
  };
}

void AsyncFileSource::on_opened(Info info, std::string error) {
  assert(_state->loop->is_in_loop_thread());
  if (_state->is_cancelled()) return;
  auto conn = _connection.lock();
  if (!conn || !conn->connected()) {
    cancel();
    return;
  }
  if (!error.empty()) {
    report_error(conn, std::move(error));
    return;
  }
  _info = std::move(info);
  _opened = true;
  try {
    if (_ready) _ready(_info);
  } catch (const std::exception& exception) {
    report_error(conn, exception.what());
  } catch (...) {
    report_error(conn, "unexpected source callback failure");
  }
}

void AsyncFileSource::report_error(const NET::TcpConnectionPtr& conn, std::string error) {
  cancel();
  try {
    if (_failed) {
      _failed(std::move(error));
    } else {
      conn->force_close();
    }
  } catch (...) {
    conn->force_close();
  }
}

AsyncFileSource::State::State(DiskExecutor& executor_arg, NET::EventLoop* loop_arg,
                               Factory factory_arg)
    : executor(executor_arg), loop(loop_arg), factory(std::move(factory_arg)) {}

AsyncFileSource::State::~State() { assert(!file); }

bool AsyncFileSource::State::is_cancelled() {
  std::lock_guard<std::mutex> lock(mutex);
  return cancelled;
}

std::error_code AsyncFileSource::State::make_error(int value) {
  return {value, std::generic_category()};
}

bool AsyncFileSource::State::launch_initialize() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (cancelled || terminal || busy) return false;
    busy = true;
    auto self = shared_from_this();
    if (executor.post([self] { self->run_initialize(); }, false)) return true;
    busy = false;
  }
  cancel();
  return false;
}

void AsyncFileSource::State::run_initialize() {
  Info info;
  std::string error;
  try {
    if (!is_cancelled()) {
      auto opened = factory();
      if (!opened.file) throw std::runtime_error("file source factory returned null");
      file = std::move(opened.file);
      info.size = file->size;
      info.tag = std::move(opened.tag);
    }
  } catch (const std::exception& exception) {
    error = exception.what();
  } catch (...) {
    error = "unexpected file open failure";
  }
  factory = {};
  bool ending;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ending = cancelled || !error.empty();
    if (ending) {
      terminal = true;
      lock.unlock();
      file.reset();
      lock.lock();
    } else {
      initialized = true;
      file_size = info.size;
    }
    busy = false;
    if (post_enabled && !cancelled) {
      loop->queue_in_loop([weak = owner, info = std::move(info), error = std::move(error)]() mutable {
        if (auto source = weak.lock()) source->on_opened(std::move(info), std::move(error));
      });
    }
  }
  // No worker may post another result after its registration is released.
  if (ending) executor.release(token);
}

void AsyncFileSource::State::request(off_t offset, std::size_t count, Completion callback) {
  assert(loop->is_in_loop_thread());
  std::unique_lock<std::mutex> lock(mutex);
  if (cancelled || terminal) {
    post_read_locked(std::move(callback), make_error(ECANCELED), {});
    return;
  }
  if (!initialized || busy) {
    post_read_locked(std::move(callback), make_error(EBUSY), {});
    return;
  }
  if (offset < 0 || count == 0 || count > NET::TcpConnection::kAsyncFileChunk ||
      static_cast<std::uint64_t>(offset) > file_size ||
      count > file_size - static_cast<std::uint64_t>(offset)) {
    post_read_locked(std::move(callback), make_error(EINVAL), {});
    return;
  }
  busy = true;
  completion = std::move(callback);
  auto self = shared_from_this();
  if (executor.post([self, offset, count] { self->run_read(offset, count); }, false)) return;
  busy = false;
  post_read_locked(std::exchange(completion, Completion{}), make_error(ECANCELED), {});
  lock.unlock();
  cancel();
}

void AsyncFileSource::State::run_read(off_t offset, std::size_t count) {
  std::string data;
  std::error_code error;
  try {
    if (is_cancelled()) {
      error = make_error(ECANCELED);
    } else {
      data.resize(count);
      std::size_t done = 0;
      while (done < count) {
        if (is_cancelled()) {
          error = make_error(ECANCELED);
          break;
        }
        const ssize_t n = ::pread(file->file.fd, data.data() + done, count - done,
                                  offset + static_cast<off_t>(done));
        if (n > 0) {
          done += static_cast<std::size_t>(n);
        } else if (n == -1 && errno == EINTR) {
          continue;
        } else {
          error = make_error(n == 0 ? EIO : errno);
          break;
        }
      }
    }
  } catch (const std::bad_alloc&) {
    error = make_error(ENOMEM);
  } catch (...) {
    error = make_error(EIO);
  }
  if (error) std::string{}.swap(data);

  bool ending;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ending = cancelled || static_cast<bool>(error);
    if (ending) {
      terminal = true;
      lock.unlock();
      file.reset();
      lock.lock();
    }
    busy = false;
    if (completion) {
      post_read_locked(std::exchange(completion, Completion{}), error, std::move(data));
    }
  }
  if (ending) executor.release(token);
  // On success a new read may already be scheduled; don't access file here.
}

void AsyncFileSource::State::post_read_locked(Completion callback, std::error_code error,
                                              std::string data) {
  if (!callback) return;
  auto weak = weak_from_this();
  loop->queue_in_loop([weak, callback = std::move(callback), error,
                       data = std::move(data)]() mutable {
    auto state = weak.lock();
    if (!state || state->is_cancelled()) {
      error = make_error(ECANCELED);
      std::string{}.swap(data);
    }
    callback(error, std::move(data));
  });
}

void AsyncFileSource::State::cancel() noexcept {
  std::shared_ptr<State> self;
  {
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = true;
    post_enabled = false;
    if (completion) {
      // Deliver cancellation even while the worker is blocked inside pread.
      post_read_locked(std::exchange(completion, Completion{}), make_error(ECANCELED), {});
    }
    if (token == 0 || terminal || busy) return;
    busy = true;
    self = shared_from_this();
  }
  const bool accepted = executor.post([self] {
    {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->terminal = true;
    }
    self->file.reset();
    self->factory = {};
    {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->busy = false;
    }
    self->executor.release(self->token);
  }, true);
  if (!accepted) std::terminate();
}

}  // namespace TLSS::FileStorage::Transfer
