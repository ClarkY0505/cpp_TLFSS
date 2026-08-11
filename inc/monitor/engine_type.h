#ifndef TLSSMON_ENGINE_TYPE_H
#define TLSSMON_ENGINE_TYPE_H

#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace TLSSMON {

enum class EnginePhase : std::uint8_t {
    CREATED = 10,
    INITIALIZING,
    READY,
    RUNNING,
    STOPPING,
    STOPPED
};

struct MonConfig{
    std::string _name;
    uint16_t _port;
    uint8_t _id;
};

// is common callback
using CallbackFunction = std::function<int()>;
struct MonCallback{
    std::string _name;
    CallbackFunction _cb;
    bool _asynchronous{false};
};

// use to track the status of callback function
struct CallbackStats{
    // how many times activated
    std::uint64_t _count{0};
    // sum of durations
    std::uint64_t _total_us{0};
    std::uint64_t _min_us{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t _max_us{0};
};

} // namespace TLSSMON

#endif // TLSSMON_ENGINE_TYPE_H
