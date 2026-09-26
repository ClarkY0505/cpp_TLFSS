#ifndef TLSSMON_ENGINE_TYPE_H
#define TLSSMON_ENGINE_TYPE_H

#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace TLSSMON {

/** @brief Engine 从创建到停止的一次性生命周期阶段。 */
enum class EnginePhase : std::uint8_t {
    CREATED = 10,
    INITIALIZING,
    READY,
    RUNNING,
    STOPPING,
    STOPPED
};

/** @brief Engine 的名称、CLI 端口和节点 ID 配置。 */
struct MonConfig{
    std::string _name;
    uint16_t _port;
    uint8_t _id;
};

/** @brief 无参数监控回调；返回值由调用方解释。 */
using CallbackFunction = std::function<int()>;
/** @brief 回调名称、函数和异步执行标志。 */
struct MonCallback{
    std::string _name;
    CallbackFunction _cb;
    bool _asynchronous{false};
};

/** @brief 回调调用次数与耗时统计；耗时单位为微秒。 */
struct CallbackStats{
    /** @brief how many times activated */
    std::uint64_t _count{0};
    /** @brief sum of durations */
    std::uint64_t _total_us{0};
    std::uint64_t _min_us{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t _max_us{0};
};

} // namespace TLSSMON

#endif // TLSSMON_ENGINE_TYPE_H
