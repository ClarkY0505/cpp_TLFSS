#include "timer_manager.h"
#include "callback_registry.h"
#include "engine_type.h"
#include "timer_types.h"
#include "wake_pipe.h"
#include "engine_type.h"

#include <sys/time.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <optional>

namespace TLSSMON{
namespace Timer{
void set_idle_timeout(timeval& timeout) noexcept{
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
}

using Clock_p = std::chrono::steady_clock::time_point;
void set_timeout_until(timeval& timeout, Clock_p deadline, Clock_p now) noexcept{
    /*
     * 已到期或恰好到期时，不允许 select() 阻塞。
     */
    if (deadline <= now) {
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        return;
    }

    const auto remaining = deadline - now;
    /*
     * 向上取整到微秒，避免 duration_cast 截断导致 select()
     * 比真正的 deadline 提前醒来。
     */
    const auto remaining_us =
        std::chrono::ceil<std::chrono::microseconds>(remaining);
    constexpr std::int64_t microseconds_per_second = 1'000'000;
    const std::int64_t total_us = remaining_us.count();

    timeout.tv_sec =
        static_cast<decltype(timeout.tv_sec)>(total_us / microseconds_per_second);
    timeout.tv_usec =
        static_cast<decltype(timeout.tv_usec)>(total_us % microseconds_per_second);
}
};
struct TimerEntry {
    std::uint64_t _id{0};
    TimerFlags _flags{TimerFlags::ONCE};

    std::chrono::steady_clock::time_point _next;
    std::chrono::milliseconds _interval{0};

    std::uint64_t _fire_count{0};
    std::unique_ptr<EnhancedCallback> _cb;
};

TimerManager::TimerManager(CallbackRegistry& registry, WakeupPipe& wakeup):_registry(registry), _wakeup(wakeup){}

TimerManager::~TimerManager(){
    cleanup();
}

void TimerManager::cleanup(){
    std::list<std::unique_ptr<TimerEntry>> timers;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_cleaned_up) {
            return;
        }

        _cleaned_up = true;
        timers.splice(timers.end(), _timers);
    }

    for (auto& timer : timers) {
        if (timer && timer->_cb) {
            _registry.remove(timer->_cb.get());
        }
    }
}

TimerManager::TimerList::iterator TimerManager::insert_sorted_locked(std::unique_ptr<TimerEntry> timer){
    if(!timer){
        return _timers.end();
    }

    const auto pos = find_insert_position_locked(timer->_next);
    return _timers.insert(pos,std::move(timer));
}

TimerManager::TimerList::iterator TimerManager::find_insert_position_locked(std::chrono::steady_clock::time_point deadline){
    auto pos = _timers.begin();

    /* 只在新 deadline 严格早于当前节点时停止。
     *
     * 如果 deadline 相同，则继续向后查找，使新节点插入
     */
    while(pos != _timers.end()){
        if(!*pos || deadline < (*pos)->_next){
            break;
        }

        ++pos;
    }
    return pos;
}

void TimerManager::reinsert_sorted_locked(TimerList& active){
    if(active.empty()){
        return;
    }

    auto timer_pos = active.begin();
    if(!*timer_pos){
        return;
    }

    const auto insert_pos = find_insert_position_locked((*timer_pos)->_next);
    /*
     * 移动现有 list 节点，不创建新的 list 节点，
     * 因此不会因内存分配失败而丢失 TimerEntry。
     */
    _timers.splice(insert_pos, active, timer_pos);
}
std::optional<TimerHandle> TimerManager::add(MonCallback mcb, TimerFlags flags, std::chrono::milliseconds delay){
    // Timer 触发的间隔必须为正数
    if(delay <= std::chrono::milliseconds::zero()){
        return std::nullopt;
    }

    if(!mcb._cb){
        return std::nullopt;
    }

    if(!is_valid_timer_flags(flags)){
        return std::nullopt;
    }

    const bool recurring = 
        has_flag(flags, TimerFlags::RECURRING);
    const bool worker = 
        has_flag(flags,TimerFlags::WORKER);

    if(!recurring && worker){
        return std::nullopt;
    }

    if(_wakeup.state() != PipeState::READY){
        return std::nullopt;
    }

    std::unique_ptr<TimerEntry> entry;
    try{
        entry = std::make_unique<TimerEntry>();

        mcb._asynchronous = worker;
        entry->_flags = flags;
        entry->_interval = recurring ? delay : std::chrono::milliseconds::zero();
        entry->_cb = std::make_unique<EnhancedCallback>(std::move(mcb));

    }catch(const std::bad_alloc&){
        return std::nullopt;
    }

    using Clock = std::chrono::steady_clock;
    EnhancedCallback* const Enh_cb_p = entry->_cb.get();

    std::lock_guard<std::mutex> lock(_mutex);
    /*
     * cleanup() 一旦开始，TimerManager 就不能再接受新节点。
     * 该状态只能在持有 _mutex 时读取。
     */
    if(_cleaned_up){
        return std::nullopt;
    }
    /*
     * 0 被保留为无效 TimerHandle。
     * UINT64_MAX 可以使用一次，递增后变成 0；
     * 下一次 add() 将被这里拒绝。
     */
    if(_next_id == 0){
        return std::nullopt;
    }

    const Clock::time_point now = Clock::now();

    /*
     * 防止 now + delay 超过 steady_clock::time_point 的范围。
     */ 
    const auto maximum_delay =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);

    if(delay > maximum_delay){
        return std::nullopt;
    }

    const TimerHandle handle{_next_id};
    entry->_id = handle._id;
    entry->_next = now + std::chrono::duration_cast<Clock::duration>(delay);

    TimerList::iterator pos;
    try{
        pos = insert_sorted_locked(std::move(entry));
    }catch(const std::bad_alloc&){
        return std::nullopt;
    }

    if(pos == _timers.end()){
        return std::nullopt;
    }

    /*
     * TimerManager 已经拥有节点后，Registry 才能保存回调裸指针。
     */
    try{
        _registry.add(Enh_cb_p);
    } catch(const std::bad_alloc&){
        _timers.erase(pos);
        return std::nullopt;
    }

    /*
     * 唤醒阻塞在 select() 中的 Reactor，使其重新计算 timeout。
     */
    if(_wakeup.wakeup() != PIPESTATUS::SUCCESSFUL){
        _registry.remove(Enh_cb_p);
        _timers.erase(pos);
        return std::nullopt;
    }

    /*
     * 所有操作成功后才消费 ID。
     */
    ++_next_id;
    return handle;
}

void TimerManager::check(timeval& timeout){
    using Clock = std::chrono::steady_clock;
    /*
     * 先写入安全默认值，保证当前阶段的所有返回路径
     * 都会提供规范化的 timeval。
     */
    Timer::set_idle_timeout(timeout);

    while(true){
        TimerList active;
        std::unique_lock<std::mutex> lock(_mutex);

        if(_cleaned_up || _timers.empty()){
            return;
        }
        /*
         * _timers 的内部不变量：
         * - 不包含空 unique_ptr；
         * - 按 _next 从早到晚排列。
         */
        TimerEntry* const earliest = _timers.front().get();
        if(earliest == nullptr){
            /*
             * 防御性移除，避免解引用空指针。
             */
            _timers.pop_front();
            continue;
        }

        const Clock::time_point now = Clock::now();
        if(!(earliest->_next < now)){
            // 计算timeout
            Timer::set_timeout_until(timeout, earliest->_next, now);
            return;
        }

        active.splice(active.end(), _timers, _timers.begin());
        lock.unlock();
        // 检测上述流程是否符合预期
        assert(active.size() == 1);
        assert(active.front());

        TimerEntry& timer = *active.front();
        ++timer._fire_count;
        if(timer._cb){
            (void)timer._cb->activate();
        }

        const bool recurring = timer._interval > std::chrono::milliseconds::zero();

        assert(recurring == has_flag(timer._flags, TimerFlags::RECURRING));
        if(recurring){
            timer._next += timer._interval;
            lock.lock();
            reinsert_sorted_locked(active);
            lock.unlock();
            assert(active.empty());
            continue;
        }

        assert(!timer._cb || !timer._cb->asynchronous());

        EnhancedCallback* const Enh_cb_p = timer._cb.get();
        _registry.remove(Enh_cb_p);
        active.clear();

        assert(active.empty());
        continue;
    }
}

}
