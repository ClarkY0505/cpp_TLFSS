#ifndef __CLIENT_H__
#define __CLIENT_H__
#include "aio_types.h"
#include <unistd.h>
#include <optional>
struct ClientSession {
    int fd{-1};
    std::optional<TLSSMON::AioHandle> handle;
    ClientSession() = default;

    ~ClientSession()
    {
        close_fd();
    }

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void close_fd() noexcept
    {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }

        handle.reset();
    }
};

#endif // __CLIENT_H__
