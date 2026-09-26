#ifndef __TIMER_MANAGER_H__
#define __TIMER_MANAGER_H__

#include "timer_types.h"

#include <sys/time.h>
#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <memory>
#include <optional>
namespace TLSSMON{

class CallbackRegistry;
class WakeupPipe;
class Engine;
struct MonCallback;
struct TimerEntry;

/**
 * @brief 按到期时间管理定时器，并在事件循环中触发回调。
 *
 * @note registry 和 wakeup 由调用方持有，必须比本对象存活更久。
 */
class TimerManager{
public:
    /**
     * @brief 绑定回调注册表和事件循环唤醒管道。
     * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
     * @param wakeup 调用方持有的唤醒管道；其生命周期须覆盖当前对象。
     */
    TimerManager(CallbackRegistry& registry, WakeupPipe& wakeup);
    ~TimerManager();

    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    /**
     * @brief 注册单次或周期定时器。
     * @param mcb 非空回调；注册成功后由管理器持有。
     * @param flags 可组合 RECURRING 与 WORKER；单次定时器不能使用 WORKER。
     * @param delay 首次触发的正数延迟，也是周期定时器的间隔。
     * @return 注册成功时返回句柄，参数无效或资源操作失败时返回 std::nullopt。
     */
    std::optional<TimerHandle> add(MonCallback mcb, TimerFlags flags, std::chrono::milliseconds delay);
    /**
     * @brief 触发到期定时器，并设置下一次 select 等待时间。
     * @param timeout 输出参数；回调在管理器锁外执行。
     */
    void check(timeval& timeout);

private:
    friend class Engine;
    using TimerList = std::list<std::unique_ptr<TimerEntry>>;
    /**
     * @brief 在持锁状态下按到期时间插入定时器。
     * @param timer 要插入有序队列的定时器节点。
     * @return 新节点插入后的迭代器。
     */
    TimerList::iterator insert_sorted_locked(std::unique_ptr<TimerEntry> timer);
    /**
     * @brief 查找定时器的有序插入位置。
     * @param deadline 下一次定时器到期时间。
     * @return 按到期时间排序后应插入的位置。
     */
    TimerList::iterator find_insert_position_locked(std::chrono::steady_clock::time_point deadline);
    /**
     * @brief 将周期定时器重新放入有序队列。
     * @param active 包含待重新入队节点的活动列表。
     */
    void reinsert_sorted_locked(TimerList& active);

    void cleanup();

    CallbackRegistry& _registry;
    WakeupPipe& _wakeup;

    std::mutex _mutex;
    TimerList _timers;

    std::uint64_t _next_id{1};
    bool _cleaned_up{false};
};
}

#endif // __TIMER_MANAGER_H__
