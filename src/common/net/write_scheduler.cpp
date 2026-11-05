#include "common/net/write_scheduler.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <utility>

#include "common/net/event_loop.h"
#include "common/net/net_logger.h"

namespace TLSS::NET {
static_assert(sizeof(off_t) >= 8, "send_file requires 64-bit off_t");
namespace {
constexpr std::size_t kMaxPendingBytes = 1024 * 1024;
constexpr std::size_t kMaxPendingTasks = 1024;
constexpr std::size_t kMaxPendingFiles = 64;
constexpr std::size_t kWriteBudget = 1024 * 1024;
constexpr unsigned kWriteSteps = 64;

// sendfile 没有 MSG_NOSIGNAL 参数。
// 在当前线程临时屏蔽 SIGPIPE，清理由本次失败产生的信号，
// 最后恢复原信号掩码，不改变整个进程的信号处理方式。
ssize_t sendfile_no_sigpipe(int socket_fd, int file_fd, off_t* offset, std::size_t count) {
    sigset_t blocked;
    sigset_t old_mask;
    sigset_t pending;

    // 1. 创建一个只包含 SIGPIPE 的信号集合
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGPIPE);

    // 2. 临时屏蔽当前线程的 SIGPIPE，保存原来的掩码
    const int mask_error = ::pthread_sigmask(SIG_BLOCK, &blocked, &old_mask);

    if (mask_error != 0) {
        errno = mask_error;
        return -1;
    }

    // 3. 记录调用前是否已经有待处理的 SIGPIPE
    if (::sigpending(&pending) == -1) {
        const int saved_error = errno;
        ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
        errno = saved_error;
        return -1;
    }

    const bool already_pending = ::sigismember(&pending, SIGPIPE) == 1;

    // 4. 发送文件
    const ssize_t result = ::sendfile(socket_fd, file_fd, offset, count);
    const int saved_error = errno;

    // 5. 如果本次失败产生了 SIGPIPE，取走它
    // 不消费调用前就已经存在的 SIGPIPE
    if (result == -1 && saved_error == EPIPE && !already_pending) {
        const timespec timeout{0, 0};

        while (::sigtimedwait(&blocked, nullptr, &timeout) == -1 && errno == EINTR) {
        }
    }

    // 6. 恢复当前线程原来的信号掩码
    ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);

    // 7. 保留 sendfile 的结果及错误码
    errno = saved_error;
    return result;
}

}  // namespace

WriteScheduler::FileHandle::~FileHandle() {
    if (fd >= 0) ::close(fd);
}

WriteScheduler::WriteScheduler(EventLoop* loop, int socket_fd, Hooks hooks)
    : _loop(loop), _socket_fd(socket_fd), _hooks(std::move(hooks)) {}

WriteScheduler::~WriteScheduler() { cancel_all(ECANCELED); }

WriteScheduler::PreparedFile WriteScheduler::prepare_file(int fd, off_t offset, std::uint64_t count,
                                                          detail::FileSendCallback callback) {
    SendFileWrite task;
    task.offset = offset;
    task.remaining = count;
    task.callback = std::move(callback);
    task.file = std::make_shared<FileHandle>();
    int error = 0;
    if (offset < 0) {
        error = EINVAL;
    } else {
        int duplicate;
        do {
            duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
        } while (duplicate == -1 && errno == EINTR);
        if (duplicate == -1) {
            error = errno;
        } else {
            task.file->fd = duplicate;
            struct stat info {};
            int result;
            do {
                result = ::fstat(duplicate, &info);
            } while (result == -1 && errno == EINTR);
            if (result == -1)
                error = errno;
            else if (!S_ISREG(info.st_mode) || info.st_size < 0 || offset > info.st_size ||
                     count > static_cast<std::uint64_t>(info.st_size - offset))
                error = EINVAL;
        }
    }
    return {std::move(task), error};
}

WriteScheduler::PreparedFile WriteScheduler::prepare_async(off_t offset, std::uint64_t count,
                                                           detail::FileReadFunction reader,
                                                           detail::FileSendCallback callback) {
    AsyncReaderWrite task;
    task.offset = offset;
    task.remaining = count;
    task.reader = std::move(reader);
    task.callback = std::move(callback);
    int error = 0;
    const auto max_offset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset < 0 || !task.reader)
        error = EINVAL;
    else if (count > max_offset - static_cast<std::uint64_t>(offset))
        error = EOVERFLOW;
    return {std::move(task), error};
}

WriteScheduler::FileProgress* WriteScheduler::file_progress(WriteTask& task) {
    return std::visit(
        [](auto& write) -> FileProgress* {
            if constexpr (std::is_same_v<std::decay_t<decltype(write)>, MemoryWrite>)
                return nullptr;
            else
                return &write;
        },
        task);
}

void WriteScheduler::set_high_water_mark(std::size_t bytes) {
    if (!_loop->is_in_loop_thread() || bytes == 0 || bytes > kMaxPendingBytes) {
        net_logger()->error("invalid high water mark");
        std::abort();
    }
    _high_water_mark = bytes;
}

void WriteScheduler::set_write_interest(bool enabled) {
    if (_write_enabled == enabled) return;
    _write_enabled = enabled;
    if (_hooks.write_interest) _hooks.write_interest(enabled);
}

void WriteScheduler::enable_write_event() {
    if (_stopped || _writes.empty()) return;
    const auto* file = std::get_if<AsyncReaderWrite>(&_writes.front());
    if (file && file->read_pending) return;
    set_write_interest(true);
}

void WriteScheduler::check_high_water(std::size_t previous) {
    if (previous < _high_water_mark && _pending_memory_bytes >= _high_water_mark &&
        _hooks.high_water)
        _hooks.high_water(_pending_memory_bytes);
}

void WriteScheduler::enqueue_memory(std::string data) {
    assert(_loop->is_in_loop_thread());
    if (_stopped || data.empty()) return;
    if (_writes.size() >= kMaxPendingTasks ||
        data.size() > kMaxPendingBytes - _pending_memory_bytes) {
        fail(ENOBUFS);
        return;
    }
    const auto previous = _pending_memory_bytes;
    const auto size = data.size();
    _writes.emplace_back(MemoryWrite{std::move(data), 0});
    _pending_memory_bytes += size;
    enable_write_event();
    check_high_water(previous);
}

void WriteScheduler::enqueue_file(PreparedFile prepared, bool accepting) {
    assert(_loop->is_in_loop_thread());
    assert(file_progress(prepared.task));
    if (_stopped || !accepting) {
        notify_file(std::move(prepared.task), ENOTCONN);
        return;
    }
    int error = prepared.error;
    std::size_t reservation = 0;
    if (auto* async = std::get_if<AsyncReaderWrite>(&prepared.task)) {
        reservation = static_cast<std::size_t>(
            std::min<std::uint64_t>(async->remaining, detail::kAsyncFileChunk));
        async->reservation = reservation;
    }
    if (!error && (_writes.size() >= kMaxPendingTasks || _pending_files >= kMaxPendingFiles ||
                   reservation > kMaxPendingBytes - _pending_memory_bytes))
        error = ENOBUFS;
    if (error) {
        notify_file(std::move(prepared.task), error);
        fail(error);
        return;
    }
    const auto previous = _pending_memory_bytes;
    _writes.push_back(std::move(prepared.task));
    ++_pending_files;
    _pending_memory_bytes += reservation;
    enable_write_event();
    check_high_water(previous);
}

void WriteScheduler::request_file_read() {
    if (_next_read_sequence == std::numeric_limits<std::uint64_t>::max()) {
        fail(EOVERFLOW);
        return;
    }
    auto& task = std::get<AsyncReaderWrite>(_writes.front());
    assert(!task.read_pending && task.data.empty() && task.remaining != 0);
    const auto sequence = ++_next_read_sequence;
    const auto offset = task.offset;
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(task.remaining, detail::kAsyncFileChunk));
    auto reader = task.reader;
    task.read_pending = true;
    task.read_sequence = sequence;
    task.read_size = count;
    set_write_interest(false);
    auto completion = _hooks.make_read_completion(sequence);
    // Calling an external reader may cancel the queue. Do not access task afterwards.
    try {
        reader(offset, count, std::move(completion));
    } catch (const std::system_error& exception) {
        fail(exception.code().value() != 0 ? exception.code().value() : EIO);
    } catch (...) {
        fail(EIO);
    }
}

void WriteScheduler::on_file_read(std::uint64_t sequence, std::error_code error, std::string data) {
    assert(_loop->is_in_loop_thread());
    if (_stopped || _writes.empty()) return;
    auto* task = std::get_if<AsyncReaderWrite>(&_writes.front());
    if (!task || !task->read_pending || task->read_sequence != sequence) return;
    task->read_pending = false;
    if (error) {
        fail(error.value() != 0 ? error.value() : EIO);
        return;
    }
    if (data.size() != task->read_size || data.size() > task->remaining ||
        data.size() > task->reservation) {
        fail(EIO);
        return;
    }
    task->data = std::move(data);
    task->data_offset = 0;
    enable_write_event();
}

void WriteScheduler::notify_file(WriteTask task, int error) {
    auto* progress = file_progress(task);
    assert(progress);
    auto callback = std::move(progress->callback);
    const auto sent = progress->sent;
    // Release reader/file references before posting the application callback.
    if (auto* file = std::get_if<SendFileWrite>(&task))
        file->file.reset();
    else
        std::get<AsyncReaderWrite>(task).reader = {};
    if (callback) {
        const auto result =
            error ? std::error_code(error, std::generic_category()) : std::error_code{};
        _loop->queue_in_loop(
            [callback = std::move(callback), result, sent] { callback(result, sent); });
    }
}

void WriteScheduler::complete_front() {
    auto task = std::move(_writes.front());
    _writes.pop_front();
    if (const auto* memory = std::get_if<MemoryWrite>(&task)) {
        _pending_memory_bytes -= memory->data.size();
        return;
    }
    if (const auto* async = std::get_if<AsyncReaderWrite>(&task))
        _pending_memory_bytes -= async->reservation;
    --_pending_files;
    notify_file(std::move(task), 0);
}

void WriteScheduler::on_writable() {
    assert(_loop->is_in_loop_thread());
    if (_stopped || !_write_enabled) return;
    std::size_t budget = kWriteBudget;
    unsigned steps = 0;
    while (!_writes.empty() && budget > 0 && steps < kWriteSteps) {
        ++steps;
        auto& task = _writes.front();
        auto* progress = file_progress(task);
        if (progress && progress->remaining == 0) {
            complete_front();
            continue;
        }
        auto* async = std::get_if<AsyncReaderWrite>(&task);
        if (async && async->data.empty()) {
            if (async->read_pending)
                set_write_interest(false);
            else
                request_file_read();
            return;
        }
        auto* memory = std::get_if<MemoryWrite>(&task);
        ssize_t result;
        if (auto* file = std::get_if<SendFileWrite>(&task)) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(file->remaining, budget));
            result = sendfile_no_sigpipe(_socket_fd, file->file->fd, &file->offset, count);
        } else {
            const auto& data = memory ? memory->data : async->data;
            const auto offset = memory ? memory->offset : async->data_offset;
            result = ::send(_socket_fd, data.data() + offset,
                            std::min(data.size() - offset, budget), MSG_NOSIGNAL);
        }
        if (result > 0) {
            const auto written = static_cast<std::size_t>(result);
            budget -= written;
            bool completed;
            if (memory) {
                memory->offset += written;
                completed = memory->offset == memory->data.size();
            } else {
                progress->remaining -= written;
                progress->sent += written;
                if (async) {
                    async->offset += static_cast<off_t>(written);
                    async->data_offset += written;
                    if (async->data_offset == async->data.size()) {
                        std::string{}.swap(async->data);
                        async->data_offset = 0;
                    }
                }
                completed = progress->remaining == 0;
            }
            if (completed) complete_front();
            continue;
        }
        if (result == 0) {
            fail(EIO);
            return;
        }
        const int error = errno;
        if (error == EINTR) continue;
        if (error == EAGAIN || error == EWOULDBLOCK) return;
        fail(error);
        return;
    }
    if (_writes.empty()) {
        set_write_interest(false);
        if (_hooks.drained) _hooks.drained();
    }
}

void WriteScheduler::cancel_all(int error) {
    _stopped = true;
    set_write_interest(false);
    auto writes = std::move(_writes);
    _writes.clear();
    _pending_memory_bytes = 0;
    _pending_files = 0;
    for (auto& task : writes) {
        if (file_progress(task)) notify_file(std::move(task), error);
    }
}

void WriteScheduler::fail(int error) {
    if (_stopped) return;
    cancel_all(error);
    if (_hooks.failed) _hooks.failed(error);
}
}  // namespace TLSS::NET
