#ifndef __CLIENT_H__
#define __CLIENT_H__
#include "aio_types.h"
#include <unistd.h>
#include <optional>
/**
 * @brief 独立持有客户端 socket，并记录对应的 AIO 注册句柄。
 * @note 析构时关闭 fd；移除 AIO 注册项仍由调用方负责。
 */
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

    /** @brief 幂等关闭 socket 并清除保存的 AIO 句柄。 */
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
