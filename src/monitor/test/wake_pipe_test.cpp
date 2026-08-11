#include "wake_pipe.h"

#include <cassert>
#include <fcntl.h>
#include <thread>
#include <vector>

using TLSSMON::PIPESTATUS;
using TLSSMON::PipeState;
using TLSSMON::WakeupPipe;

int main()
{
    WakeupPipe pipe;
    assert(pipe.state() == PipeState::UNINITIALIZED);
    assert(pipe.read_fd() == -1);

    assert(pipe.init() == PIPESTATUS::SUCCESSFUL);
    assert(pipe.state() == PipeState::READY);
    assert(pipe.read_fd() >= 0);
    assert(pipe.init() == PIPESTATUS::ALREADYINITIALIZED);

    const int status_flags = ::fcntl(pipe.read_fd(), F_GETFL, 0);
    assert(status_flags >= 0);
    assert((status_flags & O_NONBLOCK) != 0);

    const int descriptor_flags = ::fcntl(pipe.read_fd(), F_GETFD, 0);
    assert(descriptor_flags >= 0);
    assert((descriptor_flags & FD_CLOEXEC) != 0);

    assert(pipe.wakeup() == PIPESTATUS::SUCCESSFUL);
    assert(pipe.drain() == PIPESTATUS::SUCCESSFUL);

    std::vector<std::thread> writers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        writers.emplace_back([&pipe] {
            for (int write_index = 0; write_index < 4096; ++write_index) {
                assert(pipe.wakeup() == PIPESTATUS::SUCCESSFUL);
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    assert(pipe.drain() == PIPESTATUS::SUCCESSFUL);
}
