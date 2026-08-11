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

class AioManager {
public:
    AioManager(CallbackRegistry& registry, WakeupPipe& wakeup);
    ~AioManager();

    AioManager(const AioManager&) = delete;
    AioManager& operator=(const AioManager&) = delete;

    std::optional<AioHandle> add(int fd, MonCallback cb);

    bool remove(AioHandle handle);

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
