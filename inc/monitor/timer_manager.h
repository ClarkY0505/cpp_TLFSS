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

class TimerManager{
public:
    TimerManager(CallbackRegistry& registry, WakeupPipe& wakeup);
    ~TimerManager();

    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    std::optional<TimerHandle> add(MonCallback mcb, TimerFlags flags, std::chrono::milliseconds delay);
    void check(timeval& timeout);

private:
    friend class Engine;
    using TimerList = std::list<std::unique_ptr<TimerEntry>>;
    TimerList::iterator insert_sorted_locked(std::unique_ptr<TimerEntry> timer);
    TimerList::iterator find_insert_position_locked(std::chrono::steady_clock::time_point deadline);
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
