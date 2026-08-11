#ifndef TLSSMON_ENGINE_H
#define TLSSMON_ENGINE_H

#include "aio_types.h"
#include "aio_manager.h"
#include "callback_registry.h"
#include "engine_type.h"
#include "wake_pipe.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace TLSSMON {

enum class ENGINESTATE : int{
    INITFAILED         = -500,
    PIPEINITERR        = -501,
    ALREADYINITIALIZED = -502,
    INVALIDCONFIG      = -503,
    NOTREADY           = -504,
    ALREADYRUNNING     = -505,
    WAITFAILED         = -506,
    PIPEERROR          = -507,
    AIOINITERR         = -508, /* AIOINITERR 表示 WakeupPipe 已创建，但将其读端注册进 AIO 失败*/
    SUCCESSFUL         = 0
};

struct MonContext final {
    /**
     * @brief 根据监控配置创建运行上下文，并建立 AIO 管理器与回调注册表、
     *        唤醒管道之间的关联。
     *        Creates the runtime context from the monitoring configuration and
     *        connects the AIO manager with the callback registry and wakeup pipe.
     *
     * @param config[in] 监控配置。配置内容会被移动到上下文中。
     *               Monitoring configuration whose contents are moved into the context.
     */
    explicit MonContext(MonConfig config)
        : _name(std::move(config._name)),
        _cli_port(config._port),
        _software_id(config._id),
        _aio(_cbs,_wakeup)
    {
    }

    MonContext(const MonContext&) = delete;
    MonContext& operator=(const MonContext&) = delete;

    const std::string _name;
    const std::uint16_t _cli_port;
    const std::uint8_t _software_id;

    CallbackRegistry _cbs;
    WakeupPipe _wakeup;
    AioManager _aio;

    std::optional<AioHandle> _wakeup_handle;
};

/**
 * @brief 监控引擎的核心类，负责初始化资源、运行事件循环以及管理 AIO 回调。
 *        Core monitoring engine responsible for resource initialization,
 *        event-loop execution, and AIO callback management.
 *
 * @note Engine 采用一次性生命周期：
 *       CREATED -> INITIALIZING -> READY -> RUNNING -> STOPPING -> STOPPED。
 *       Engine has a one-shot lifecycle:
 *       CREATED -> INITIALIZING -> READY -> RUNNING -> STOPPING -> STOPPED.
 *
 * @note 进入 STOPPED 状态后不能重新运行。
 *       The engine cannot be restarted after entering STOPPED.
 */
class Engine {
public:
    /**
     * @brief 创建处于 CREATED 状态的 Engine。
     *        Constructs an Engine in the CREATED phase.
     *
     * @param config Engine 初始化时使用的监控配置。
     *               Monitoring configuration used during initialization.
     *
     * @note 构造函数不会立即创建唤醒管道或 AIO 资源；这些资源由 init() 创建。
     *       The constructor does not create the wakeup pipe or AIO resources;
     *       those resources are created by init().
     */
    explicit Engine(MonConfig config);
    ~Engine();

    /**
     * @brief 初始化 Engine 的运行上下文、唤醒管道和 AIO 管理器。
     *        Initializes the Engine runtime context, wakeup pipe, and AIO manager.
     *
     * @return 初始化结果。成功后 Engine 进入 READY 状态；可恢复的初始化失败会使
     *         Engine 回到 CREATED 状态。
     *         Initialization result. On success, the Engine enters READY;
     *         recoverable initialization failures restore CREATED.
     *
     * @retval ENGINESTATE::SUCCESSFUL 初始化成功。
     *         Initialization succeeded.
     * @retval ENGINESTATE::ALREADYINITIALIZED Engine 不处于 CREATED 状态。
     *         The Engine is not in CREATED.
     * @retval ENGINESTATE::INVALIDCONFIG 配置无效；当前实现仅检查名称是否为空。
     *         The configuration is invalid; currently only an empty name is checked.
     * @retval ENGINESTATE::PIPEINITERR 唤醒管道初始化失败。
     *         Wakeup-pipe initialization failed.
     * @retval ENGINESTATE::AIOINITERR 无法将唤醒管道读端注册到 AIO 管理器。
     *         The wakeup pipe's read end could not be registered with the AIO manager.
     * @retval ENGINESTATE::INITFAILED 初始化期间发生内存分配失败。
     *         Memory allocation failed during initialization.
     */
    ENGINESTATE init();
    /**
     * @brief 启动并阻塞执行 Engine 事件循环，直到 stop() 请求停止或等待失败。
     *        Starts and blocks in the Engine event loop until stop() requests
     *        termination or an event-wait operation fails.
     *
     * @return 事件循环的结束状态。函数返回后，已成功启动的 Engine 将进入
     *         STOPPED 状态。
     *         Final event-loop status. After a successfully started loop returns,
     *         the Engine is in STOPPED.
     *
     * @retval ENGINESTATE::SUCCESSFUL 收到停止请求并正常退出。
     *         A stop request was received and the loop exited normally.
     * @retval ENGINESTATE::ALREADYRUNNING Engine 已经处于 RUNNING 状态。
     *         The Engine is already RUNNING.
     * @retval ENGINESTATE::NOTREADY Engine 未完成初始化、已经停止，或运行上下文不存在。
     *         The Engine is not initialized, has already stopped, or has no runtime context.
     * @retval ENGINESTATE::PIPEERROR 内部唤醒管道的文件描述符无效。
     *         The internal wakeup-pipe descriptor is invalid.
     * @retval ENGINESTATE::WAITFAILED AIO 等待或处理过程失败。
     *         The AIO wait or processing operation failed.
     *
     * @note 该函数通常应在专用运行线程中调用。
     *       This function should normally be called from a dedicated runner thread.
     */
    ENGINESTATE run();
    void stop();

    EnginePhase get_phase() const noexcept;
    /**
     * @brief 向事件循环注册文件描述符及其回调。
     *        Registers a file descriptor and its callback with the event loop.
     *
     * @param fd 要监听的文件描述符，必须位于 [0, FD_SETSIZE) 范围内，并且不能是
     *           Engine 内部唤醒管道的读端。
     *           Descriptor to monitor. It must be in [0, FD_SETSIZE) and must not
     *           be the read end of the Engine's internal wakeup pipe.
     * @param cb 文件描述符就绪时执行的回调；回调对象会被移动到 AIO 管理器中。
     *           Callback executed when the descriptor becomes ready; it is moved
     *           into the AIO manager.
     *
     * @return 注册成功时返回可用于移除注册项的 AioHandle；失败时返回 std::nullopt。
     *         Returns an AioHandle for removing the registration on success,
     *         or std::nullopt on failure.
     *
     * @note 只有处于 READY 或 RUNNING 状态的 Engine 才接受注册。
     *       Registrations are accepted only while the Engine is READY or RUNNING.
     *
     * @note Engine 不接管 fd 的所有权，调用方必须保证其有效期并负责关闭。
     *       The Engine does not own fd; the caller must keep it valid and close it.
     */
    std::optional<AioHandle> add_aio(int fd, MonCallback cb);
    /**
     * @brief 根据句柄请求移除一个 AIO 注册项。
     *        Requests removal of an AIO registration by handle.
     *
     * @param handle add_aio() 返回的注册句柄。
     *               Registration handle returned by add_aio().
     *
     * @return 接受移除请求时返回 true；句柄无效、注册项不存在、已经等待移除或
     *         无法唤醒事件循环时返回 false。
     *         Returns true when the removal request is accepted; returns false
     *         if the handle is invalid, missing, already pending removal, or the
     *         event loop cannot be awakened.
     *
     * @note 底层 AIO 管理器采用延迟移除：请求成功不表示回调对象已经立即销毁。
     *       The underlying AIO manager removes entries lazily; success does not
     *       mean the callback object has already been destroyed.
     */
    bool remove_aio(AioHandle handle);
private:
    // use to control a,b,c and switch operating status
    std::mutex _control_mutex;

    MonConfig _config;
    std::unique_ptr<MonContext> _context;
    std::atomic<EnginePhase> _phase{EnginePhase::CREATED};
};

} // namespace TLSSMON

#endif // TLSSMON_ENGINE_H
