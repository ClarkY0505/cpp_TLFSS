#ifndef TLSSMON_WAKE_PIPE_H
#define TLSSMON_WAKE_PIPE_H

#include <atomic>

namespace TLSSMON {

enum class PIPESTATUS : int {
    OPENFAILED = -600,
    WAKEUPFAILED = -601,
    READFAILED = -602,
    PIPECLOSED = -603,
    SETNONBLOCKFAILED = -604,
    SETCLOSEONEXECFAILED = -605,
    ALREADYINITIALIZED = -606,
    SUCCESSFUL = 0
};

enum class PipeState {
    UNINITIALIZED,
    INITIALIZING,
    READY,
    CLOSED
};

class WakeupPipe {
public:
    WakeupPipe() noexcept = default;
    ~WakeupPipe();

    WakeupPipe(const WakeupPipe&) = delete;
    WakeupPipe& operator=(const WakeupPipe&) = delete;

    PIPESTATUS init() noexcept;
    PIPESTATUS wakeup() const noexcept;
    PIPESTATUS drain() const noexcept;

    int read_fd() const noexcept;
    PipeState state() const noexcept;

private:
    static PIPESTATUS set_non_blocking(int fd) noexcept;
    static PIPESTATUS set_close_on_exec(int fd) noexcept;

    int _read_fd{-1};
    int _write_fd{-1};
    std::atomic<PipeState> _state{PipeState::UNINITIALIZED};
};

} // namespace TLSSMON

#endif // TLSSMON_WAKE_PIPE_H
