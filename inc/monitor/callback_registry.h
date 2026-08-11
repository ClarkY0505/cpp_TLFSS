#ifndef TLSSMON_CALLBACK_REGISTRY_H
#define TLSSMON_CALLBACK_REGISTRY_H

#include "engine_type.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace TLSSMON {

// EnhancedCallback was MonCallback,
// now the enhanced wrapper so aio activations share the same
// stats/thread path as timers.
class EnhancedCallback {
public:
    explicit EnhancedCallback(MonCallback cb);
    ~EnhancedCallback();

    EnhancedCallback(const EnhancedCallback&) = delete;
    EnhancedCallback& operator=(const EnhancedCallback&) = delete;

    int activate();
    void stop_worker();
    bool asynchronous() const noexcept;

    const std::string& name() const noexcept;
    CallbackStats stats() const;

private:
    int invoke_and_record() noexcept;
    void worker_loop() noexcept;

    MonCallback _mcb;

    mutable std::mutex _stats_mutex;
    CallbackStats _stats;

    std::mutex _mutex;
    std::thread _worker;
    std::condition_variable _condition;
    bool _worker_running{false};
    bool _pending{false};
    bool _stopping{false};
};

// The registry does not own the callbacks; it only tracks traversal and
// stops workers. Ownership is held by the AIO/Timer nodes.
class CallbackRegistry {
public:
    void add(EnhancedCallback* cb);
    void remove(EnhancedCallback* cb);

    void stop_workers();
    void print_stats() const;

private:
    mutable std::mutex _mutex;
    std::vector<EnhancedCallback*> _cbs;
};

} // namespace TLSSMON

#endif // TLSSMON_CALLBACK_REGISTRY_H
