#ifndef TLSSMON_CALLBACK_REGISTRY_H
#define TLSSMON_CALLBACK_REGISTRY_H

#include "engine_type.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace TLSSMON {

/**
 * @brief 为 AIO 和定时器回调统一提供耗时统计和异步 worker。
 * @note 异步模式合并待处理的激活请求，不为每次激活新建线程。
 */
class EnhancedCallback {
public:
    /**
     * @brief 接管回调函数和名称。
     * @param cb 要执行或注册的回调。
     */
    explicit EnhancedCallback(MonCallback cb);
    ~EnhancedCallback();

    EnhancedCallback(const EnhancedCallback&) = delete;
    EnhancedCallback& operator=(const EnhancedCallback&) = delete;

    /**
     * @brief 同步执行回调，或通知异步 worker 执行。
     * @return 同步模式返回回调结果；异步激活成功返回 0，停止或启动失败返回 -1。
     */
    int activate();
    /** @brief 请求停止异步 worker 并等待其退出；可重复调用。 */
    void stop_worker();
    /**
     * @brief 返回回调是否使用异步 worker。
     * @return 使用异步 worker 时返回 true。
     */
    bool asynchronous() const noexcept;

    /**
     * @brief 返回回调名称的引用。
     * @return 回调名称或配置名称的引用。
     */
    const std::string& name() const noexcept;
    /**
     * @brief 返回调用次数和耗时统计的副本。
     * @return 当前统计信息的独立快照。
     */
    CallbackStats stats() const;

private:
    /**
     * @brief 执行回调并记录调用次数与耗时。
     * @return 回调的返回值；回调抛异常时返回 -1。
     */
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

/**
 * @brief 跟踪回调以统一停止 worker 和输出统计。
 * @note 不拥有回调；实际所有权属于 AIO 或定时器节点。
 */
class CallbackRegistry {
public:
    /**
     * @brief 登记由外部节点持有的回调。
     * @param cb 要执行或注册的回调。
     */
    void add(EnhancedCallback* cb);
    /**
     * @brief 注销回调；调用方负责其生命周期。
     * @param cb 要执行或注册的回调。
     */
    void remove(EnhancedCallback* cb);

    /** @brief 停止已登记回调的异步 worker。 */
    void stop_workers();
    /** @brief 输出当前回调统计。 */
    void print_stats() const;

private:
    mutable std::mutex _mutex;
    std::vector<EnhancedCallback*> _cbs;
};

} // namespace TLSSMON

#endif // TLSSMON_CALLBACK_REGISTRY_H
