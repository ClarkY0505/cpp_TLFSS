#ifndef TLSSMON_WAKE_PIPE_H
#define TLSSMON_WAKE_PIPE_H

#include <atomic>

namespace TLSSMON {

class Engine;

/** @brief 唤醒管道初始化与读写操作的结果。 */
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

/** @brief 唤醒管道的一次性生命周期状态。 */
enum class PipeState {
    UNINITIALIZED,
    INITIALIZING,
    READY,
    CLOSED
};

/** @brief 使用非阻塞管道唤醒等待文件描述符的事件循环。 */
class WakeupPipe {
public:
    WakeupPipe() noexcept = default;
    ~WakeupPipe();

    WakeupPipe(const WakeupPipe&) = delete;
    WakeupPipe& operator=(const WakeupPipe&) = delete;

    /**
     * @brief 创建管道并设置非阻塞和 close-on-exec；重复调用返回 ALREADYINITIALIZED。
     * @return 操作状态码。
     */
    PIPESTATUS init() noexcept;
    /**
     * @brief 写入唤醒信号；管道已满也视为已成功唤醒。
     * @return 操作状态码。
     */
    PIPESTATUS wakeup() const noexcept;
    /**
     * @brief 读尽积压的唤醒信号。
     * @return 操作状态码。
     */
    PIPESTATUS drain() const noexcept;

    /**
     * @brief 返回供事件循环监听的读端；未就绪时返回 -1。
     * @return 可供事件循环监听的读端；未就绪时返回 -1。
     */
    int read_fd() const noexcept;
    /**
     * @brief 返回当前生命周期状态。
     * @return 当前唤醒管道状态。
     */
    PipeState state() const noexcept;

private:
    friend class Engine;
    void pipe_close() noexcept;
    /**
     * @brief 将文件描述符设置为非阻塞。
     * @param fd 文件描述符；所有权仍由调用方管理。
     * @return 操作状态码。
     */
    static PIPESTATUS set_non_blocking(int fd) noexcept;
    /**
     * @brief 设置文件描述符的 close-on-exec 标志。
     * @param fd 文件描述符；所有权仍由调用方管理。
     * @return 操作状态码。
     */
    static PIPESTATUS set_close_on_exec(int fd) noexcept;

    int _read_fd{-1};
    int _write_fd{-1};
    std::atomic<PipeState> _state{PipeState::UNINITIALIZED};
};

} // namespace TLSSMON

#endif // TLSSMON_WAKE_PIPE_H
