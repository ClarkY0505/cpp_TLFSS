#include "wake_pipe.h"

#include <atomic>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace TLSSMON {

WakeupPipe::~WakeupPipe()
{   
    pipe_close();
}

PIPESTATUS WakeupPipe::init() noexcept
{
    PipeState expected = PipeState::UNINITIALIZED;
    if (!_state.compare_exchange_strong(
            expected,
            PipeState::INITIALIZING,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return PIPESTATUS::ALREADYINITIALIZED;
    }

    int fds[2]{-1, -1};
    if (::pipe(fds) == -1) {
        _state.store(PipeState::UNINITIALIZED, std::memory_order_release);
        return PIPESTATUS::OPENFAILED;
    }

    PIPESTATUS result = set_non_blocking(fds[0]);
    if (result == PIPESTATUS::SUCCESSFUL) {
        result = set_non_blocking(fds[1]);
    }
    if (result == PIPESTATUS::SUCCESSFUL) {
        result = set_close_on_exec(fds[0]);
    }
    if (result == PIPESTATUS::SUCCESSFUL) {
        result = set_close_on_exec(fds[1]);
    }

    if (result != PIPESTATUS::SUCCESSFUL) {
        ::close(fds[0]);
        ::close(fds[1]);
        _state.store(PipeState::UNINITIALIZED, std::memory_order_release);
        return result;
    }

    _read_fd = fds[0];
    _write_fd = fds[1];
    _state.store(PipeState::READY, std::memory_order_release);
    return PIPESTATUS::SUCCESSFUL;
}

int WakeupPipe::read_fd() const noexcept
{
    if (state() != PipeState::READY) {
        return -1;
    }

    return _read_fd;
}

PipeState WakeupPipe::state() const noexcept
{
    return _state.load(std::memory_order_acquire);
}

PIPESTATUS WakeupPipe::wakeup() const noexcept
{
    if (state() != PipeState::READY) {
        return PIPESTATUS::PIPECLOSED;
    }

    constexpr char signal = '*';
    while (true) {
        const auto result = ::write(_write_fd, &signal, sizeof(signal));
        if (result == sizeof(signal)) {
            return PIPESTATUS::SUCCESSFUL;
        }

        if (result == -1 && errno == EINTR) {
            continue;
        }

        if (result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return PIPESTATUS::SUCCESSFUL;
        }

        return PIPESTATUS::WAKEUPFAILED;
    }
}

PIPESTATUS WakeupPipe::drain() const noexcept
{
    if (state() != PipeState::READY) {
        return PIPESTATUS::PIPECLOSED;
    }

    char buffer[64];
    while (true) {
        const auto result = ::read(_read_fd, buffer, sizeof(buffer));
        if (result > 0) {
            continue;
        }

        if (result == 0) {
            return PIPESTATUS::PIPECLOSED;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return PIPESTATUS::SUCCESSFUL;
        }

        return PIPESTATUS::READFAILED;
    }
}

PIPESTATUS WakeupPipe::set_non_blocking(int fd) noexcept
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return PIPESTATUS::SETNONBLOCKFAILED;
    }

    return PIPESTATUS::SUCCESSFUL;
}

PIPESTATUS WakeupPipe::set_close_on_exec(int fd) noexcept
{
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags == -1 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        return PIPESTATUS::SETCLOSEONEXECFAILED;
    }

    return PIPESTATUS::SUCCESSFUL;
}

void WakeupPipe::pipe_close() noexcept{
    const PipeState previous = _state.exchange(PipeState::CLOSED, std::memory_order_acq_rel);
    if(previous == PipeState::CLOSED){
        return;
    }

    const int read_fd = std::exchange(_read_fd, -1);
    const int write_fd = std::exchange(_write_fd, -1);

    if(read_fd >= 0){
        ::close(read_fd);
    }

    if(write_fd >= 0){
        ::close(write_fd);
    }
}


} // namespace TLSSMON
