#ifndef TLSSMON_AIO_MANAGER_H
#define TLSSMON_AIO_MANAGER_H

#include "aio_types.h"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <sys/time.h>

namespace TLSSMON {

class Engine;
class CallbackRegistry;
class WakeupPipe;
struct MonCallback;
struct AioEntry;

/**
 * @brief 管理 select 监听项及其回调，并通过唤醒管道通知事件循环。
 *
 * @note 管理器拥有注册项和回调，不拥有调用方传入的文件描述符。
 *       registry 和 wakeup 的生命周期必须覆盖本对象。
 */
class AioManager {
public:
    /**
     * @brief 引用外部回调注册表和唤醒管道。
     * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
     * @param wakeup 调用方持有的唤醒管道；其生命周期须覆盖当前对象。
     */
    AioManager(CallbackRegistry& registry, WakeupPipe& wakeup);
    ~AioManager();

    AioManager(const AioManager&) = delete;
    AioManager& operator=(const AioManager&) = delete;

    /**
     * @brief 注册可读文件描述符及回调。
     * @param fd 有效且小于 FD_SETSIZE 的描述符，所有权仍归调用方。
     * @param cb 非空回调；成功时移入管理器。
     * @return 成功时返回非零句柄，参数无效或注册失败时返回 std::nullopt。
     */
    std::optional<AioHandle> add(int fd, MonCallback cb);

    /**
     * @brief 请求移除注册项；实际回收在后续 process() 中进行。
     * @param handle 此前注册操作返回的句柄。
     * @return 请求被接受时为 true，句柄无效、重复移除或唤醒失败时为 false。
     */
    bool remove(AioHandle handle);

    /**
     * @brief 等待可读事件并激活对应回调。
     * @param timeout select 等待上限；允许为空以无限等待。
     * @return 处理成功（包括超时或 EINTR）时返回 0，等待或分配失败时返回 -1。
     */
    int process(timeval* timeout);

private:
    friend class Engine;
    void cleanup();

    CallbackRegistry& _registry;
    WakeupPipe& _wakeup;

    std::mutex _mutex;
    std::list<std::unique_ptr<AioEntry>> _entries;
    std::uint64_t _next_id{1};

    bool _cleaned_up{false};
};

} // namespace TLSSMON

#endif // TLSSMON_AIO_MANAGER_H
