#ifndef TLSS_COMMON_NET_WRITE_SCHEDULER_H
#define TLSS_COMMON_NET_WRITE_SCHEDULER_H

#include <deque>
#include <memory>
#include <variant>

#include "common/NoCopy.h"
#include "common/net/write_types.h"

namespace TLSS::NET {
class EventLoop;

// Internal, loop-confined FIFO. Borrows the socket; the connection owns its lifetime
// and translates hooks into Channel changes and deferred application callbacks.
class WriteScheduler : NoCopy {
public:
    struct Hooks {
        std::function<void(bool)> write_interest;
        std::function<void(std::size_t)> high_water;
        std::function<void()> drained;
        std::function<void(int)> failed;
        // The owner supplies weak-lifetime routing and always queues results to its loop.
        std::function<detail::FileReadCompletion(std::uint64_t)> make_read_completion;
    };

    struct FileHandle {
        explicit FileHandle(int value = -1) : fd(value) {}
        ~FileHandle();
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;
        int fd;
    };

    struct MemoryWrite {
        std::string data;
        std::size_t offset = 0;
    };
    struct FileProgress {
        off_t offset = 0;
        std::uint64_t remaining = 0;
        std::uint64_t sent = 0;
        detail::FileSendCallback callback;
    };
    struct SendFileWrite : FileProgress {
        // Shared ownership keeps the prepared task copyable for EventLoop::Functor.
        std::shared_ptr<FileHandle> file;
    };
    struct AsyncReaderWrite : FileProgress {
        detail::FileReadFunction reader;
        std::string data;
        std::size_t data_offset = 0;
        bool read_pending = false;
        std::uint64_t read_sequence = 0;
        std::size_t read_size = 0;
        std::size_t reservation = 0;
    };
    using WriteTask = std::variant<MemoryWrite, SendFileWrite, AsyncReaderWrite>;
    struct PreparedFile {
        WriteTask task;
        int error = 0;
    };

    WriteScheduler(EventLoop* loop, int socket_fd, Hooks hooks);
    ~WriteScheduler();

    // Preparation is independent of the loop. In particular, fd duplication must
    // finish before TcpConnection::send_file returns to its caller.
    static PreparedFile prepare_file(int fd, off_t offset, std::uint64_t count,
                                     detail::FileSendCallback callback);
    static PreparedFile prepare_async(off_t offset, std::uint64_t count,
                                      detail::FileReadFunction reader,
                                      detail::FileSendCallback callback);

    void enqueue_memory(std::string data);
    void enqueue_file(PreparedFile prepared, bool accepting = true);
    void on_writable();
    void on_file_read(std::uint64_t sequence, std::error_code error, std::string data);
    // Terminal and idempotent. Does not report a successful drain.
    void cancel_all(int error);

    bool empty() const { return _writes.empty(); }
    std::size_t pending_memory_bytes() const { return _pending_memory_bytes; }
    std::size_t high_water_mark() const { return _high_water_mark; }
    void set_high_water_mark(std::size_t bytes);

private:
    static FileProgress* file_progress(WriteTask& task);
    void request_file_read();
    void set_write_interest(bool enabled);
    void enable_write_event();
    void check_high_water(std::size_t previous);
    void complete_front();
    void notify_file(WriteTask task, int error);
    void fail(int error);

    EventLoop* const _loop;
    const int _socket_fd;
    Hooks _hooks;
    bool _write_enabled = false;
    bool _stopped = false;
    std::deque<WriteTask> _writes;
    // Memory tasks retain their full charge until completion. Every async file
    // reserves one bounded chunk on admission; sendfile contents are not charged.
    std::size_t _pending_memory_bytes = 0;
    std::size_t _pending_files = 0;
    std::size_t _high_water_mark = 512 * 1024;
    std::uint64_t _next_read_sequence = 0;
};
}  // namespace TLSS::NET

#endif  // TLSS_COMMON_NET_WRITE_SCHEDULER_H
