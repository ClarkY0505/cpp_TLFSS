#ifndef TLSSMON_ENGINE_H
#define TLSSMON_ENGINE_H

#include "aio_types.h"
#include "engine_type.h"
#include "monitor_data.h"
#include "monitor_module_registry.h"
#include "monitor_reporter.h"
#include "timer_types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TLSSMON {

/** @brief Engine 初始化和事件循环操作的状态码。 */
enum class ENGINESTATE : int {
  INITFAILED = -500,
  PIPEINITERR = -501,
  ALREADYINITIALIZED = -502,
  INVALIDCONFIG = -503,
  NOTREADY = -504,
  ALREADYRUNNING = -505,
  WAITFAILED = -506,
  PIPEERROR = -507,
  AIOINITERR = -508, ///< 唤醒管道已创建，但其读端注册到 AIO 失败。
  SUCCESSFUL = 0
};

struct MonContext;

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
  using MonitorPublisher = MonitorReporter::Publisher;
  using AlarmPublisher = TLSSMON::AlarmPublisher;
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
  /** @brief 释放 Engine 对象；调用方须先停止运行线程并等待其退出。 */
  ~Engine();

  /**
   * @brief 初始化 Engine 的运行上下文、唤醒管道和 AIO 管理器。
   *        Initializes the Engine runtime context, wakeup pipe, and AIO
   * manager.
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
   *         The configuration is invalid; currently only an empty name is
   * checked.
   * @retval ENGINESTATE::PIPEINITERR 唤醒管道初始化失败。
   *         Wakeup-pipe initialization failed.
   * @retval ENGINESTATE::AIOINITERR 无法将唤醒管道读端注册到 AIO 管理器。
   *         The wakeup pipe's read end could not be registered with the AIO
   * manager.
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
   * @retval ENGINESTATE::NOTREADY Engine
   * 未完成初始化、已经停止，或运行上下文不存在。 The Engine is not initialized,
   * has already stopped, or has no runtime context.
   * @retval ENGINESTATE::PIPEERROR 内部唤醒管道的文件描述符无效。
   *         The internal wakeup-pipe descriptor is invalid.
   * @retval ENGINESTATE::WAITFAILED AIO 等待或处理过程失败。
   *         The AIO wait or processing operation failed.
   *
   * @note 该函数通常应在专用运行线程中调用。
   *       This function should normally be called from a dedicated runner
   * thread.
   */
  ENGINESTATE run();
  /**
   * @brief 提交不可回滚的停止请求，并尝试唤醒事件循环。
   * @note 调用方须等待运行线程退出，才能确认 Engine 完成停止。
   */
  void stop();

  /**
   * @brief 返回当前生命周期阶段。
   * @return 当前 Engine 生命周期阶段。
   */
  EnginePhase get_phase() const noexcept;
  /**
   * @brief 返回配置中的 CLI 端口。
   * @return Engine 配置中的 CLI 端口。
   */
  std::uint16_t cli_port() const noexcept;

  /**
   * @brief 注册模块及其错误元数据。
   *
   * 只允许在 READY 阶段调用。
   *
   * CREATED：
   *   Engine 尚未初始化，拒绝。
   *
   * INITIALIZING：
   *   Engine 正在初始化，拒绝。
   *
   * READY：
   *   接受注册。
   *
   * RUNNING：
   *   模块表已经冻结，拒绝。
   *
   * STOPPING/STOPPED：
   *   Engine 不再接受新模块，拒绝。
   * @param module 要注册的模块信息。
   * @return 注册状态，包含名称、ID、级别或 Engine 阶段错误。
   */
  ModuleRegisterStatus register_module(MonitorModuleInfo module);

  /**
   * @brief 设置监控数据发布回调。
   *
   * 只有 READY 和 RUNNING 状态允许设置 Publisher。
   * STOPPING 和 STOPPED 状态拒绝修改 Publisher。
   *
   * Publisher 在 MonitorStore 解锁后同步执行。Engine 不持有
   * _control_mutex 调用 Publisher，因此 Publisher 可以重新进入 Engine。
   * @param publisher 要注册的发布回调；空回调表示注销。
   * @return 操作成功时返回 true，否则返回 false。
   */
  bool set_publisher(MonitorPublisher publisher);

  /**
   * @brief 上报计数值，并按 Store 规则更新记录。
   * @param key 监控记录的完整主键。
   * @param value 本次计数值。
   * @param description 可选的可读描述。
   * @return 写入结果；Engine 不接受请求时状态为 INVALID。
   *
   * @note Reporter 接口的停止并发语义：
   * - 在调用入口观察到 READY/RUNNING 的请求已经被接受，即使 Engine 随后
   *   进入 STOPPING，该请求仍允许执行完成。
   * - 在调用入口观察到 STOPPING/STOPPED 的新请求立即返回 INVALID。
   * - stop() 只提交停止请求；调用方应通过 runner.join() 等待 Engine 管理的
   *   Timer/AIO worker 退出。
   * - Engine 不等待外部线程直接发起的 report_*() 调用，外部线程仍应由调用方
   *   自行管理和回收。
   *
   * 不能使用 _control_mutex 包围整个 Reporter/Publisher 调用，否则 Publisher
   * 重新进入 Engine 时可能死锁。
   */
  MonData::UpdateResult report_count(MonData::MonitorKey key,
                                     std::uint32_t value,
                                     std::string description = {});
  /**
   * @brief 上报错误值；配置可靠告警通道时，先持久化再更新 Store。
   * @param key 监控记录的完整主键。
   * @param value 本次错误值。
   * @param description 可选的可读描述。
   * @return 写入或持久化结果。
   */
  MonData::UpdateResult report_error(MonData::MonitorKey key,
                                     std::uint32_t value,
                                     std::string description = {});
  /**
   * @brief 上报字符串值，并按 Store 规则更新记录。
   * @param key 监控记录的完整主键。
   * @param value 本次字符串值。
   * @param description 可选的可读描述。
   * @return 写入结果。
   */
  MonData::UpdateResult report_string(MonData::MonitorKey key,
                                      std::string value,
                                      std::string description = {});

  /**
   * @brief 向 Engine 保存一条监控数据。
   * @param data 完整监控记录。
   * @param force 是否强制写入原本会被忽略的初始零值。
   * @return 写入结果；不允许写入的阶段返回 INVALID。
   *
   * 只有 READY 和 RUNNING 状态接受写入。
   * CREATED、INITIALIZING、STOPPING 和 STOPPED 状态返回 INVALID。
   *
   * changed_at 由 Engine 在接受写入时生成。
   */
  MonData::UpdateResult update_data(MonData::MonitorData data,
                                    bool force = false);
  /**
   * @brief 使用调用方提供的变化时间保存监控数据。
   * @param data 完整监控记录。
   * @param changed_at 生产端提供的变化时间。
   * @param force 是否强制写入原本会被忽略的初始零值。
   * @return 写入结果；不允许写入的阶段返回 INVALID。
   *
   * 主要供 V2 Collector 使用。V2 数据报包含生产端 changed_at，
   * Collector 应通过此接口保留该时间。
   *
   * 只有 READY 和 RUNNING 状态接受写入。
   *
   * force 规则与 update_data() 完全相同。
   */
  MonData::UpdateResult update_data_at(MonData::MonitorData data,
                                       MonData::MonitorTimestamp changed_at,
                                       bool force = false);

  /**
   * @brief 根据完整 Key 查询一条监控记录。
   * @param key 要查询的完整主键。
   * @return 记录副本；阶段不允许读取或未找到时返回 std::nullopt。
   *
   * READY、RUNNING、STOPPING 和 STOPPED 状态允许读取。
   * CREATED 和 INITIALIZING 状态返回 std::nullopt。
   *
   * 返回结果是 Store 中记录的副本。
   */
  std::optional<MonData::StoredRecord>
  find_data(const MonData::MonitorKey &key) const;

  /**
   * @brief 根据过滤条件查询监控记录快照。
   * @param filter 要匹配的主键字段；空过滤器匹配全部记录。
   * @return 按主键顺序排列的记录副本。
   *
   * READY、RUNNING、STOPPING 和 STOPPED 状态允许读取。
   * CREATED 和 INITIALIZING 状态返回空 vector。
   *
   * 空过滤器返回全部记录，顺序保持：
   * mid -> level -> fid -> eid。
   */
  std::vector<MonData::StoredRecord>
  query_data(const MonData::MonitorFilter &filter = {}) const;

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
   * @return 注册成功时返回可用于移除注册项的 AioHandle；失败时返回
   * std::nullopt。 Returns an AioHandle for removing the registration on
   * success, or std::nullopt on failure.
   *
   * @note 只有处于 READY 或 RUNNING 状态的 Engine 才接受注册。
   *       Registrations are accepted only while the Engine is READY or RUNNING.
   *
   * @note Engine 不接管 fd 的所有权，调用方必须保证其有效期并负责关闭。
   *       The Engine does not own fd; the caller must keep it valid and close
   * it.
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

  /**
   * @brief 在 READY 或 RUNNING 阶段注册定时器。
   * @param callback 要注册的监控回调。
   * @param flags 定时器行为标志。
   * @param delay 首次触发延迟；周期定时器也将其用作间隔。
   * @return 成功时返回句柄；阶段、参数无效或注册失败时返回 std::nullopt。
   * @see TimerManager::add
   */
  std::optional<TimerHandle> set_timer(MonCallback callback, TimerFlags flags,
                                       std::chrono::milliseconds delay);
  /**
   * @brief 按 mid 查询模块；未找到时返回 std::nullopt。
   * @param mid 模块 ID。
   * @return 找到时返回独立副本，否则返回 std::nullopt。
   */
  std::optional<MonitorModuleInfo> find_module_by_id(std::uint32_t mid) const;
  /**
   * @brief 根据区分大小写的模块名查询模块。
   *
   * 所有生命周期阶段都允许读取。
   * 未找到时返回 std::nullopt。
   *
   * 返回的是独立副本。
   * @param name 区分大小写的名称。
   * @return 找到时返回独立副本，否则返回 std::nullopt。
   */
  std::optional<MonitorModuleInfo>
  find_module_by_name(std::string_view name) const;
  /**
   * @brief 根据 mid 和 eid 查询错误元数据。
   *
   * 所有生命周期阶段都允许读取。
   *
   * 以下情况返回 std::nullopt：
   *
   * - mid 未注册。
   * - 模块错误表为空。
   * - eid 超出错误表范围。
   * @param mid 模块 ID。
   * @param eid 事件 ID，即模块错误表的下标。
   * @return 找到时返回独立副本，否则返回 std::nullopt。
   */
  std::optional<MonitorErrorInfo> find_error(std::uint32_t mid,
                                             std::uint32_t eid) const;
  /**
   * @brief 返回全部模块的独立快照。
   *
   * 顺序继承 MonitorModuleRegistry::modules()：
   * 按模块名字典序排列。
   * @return 按模块名字典序排列的独立快照。
   */
  std::vector<MonitorModuleInfo> modules() const;

  /**
   * @brief 注册或注销可靠告警 Publisher。
   *
   * READY、RUNNING：
   *   允许注册或注销。
   *
   * CREATED、INITIALIZING、STOPPING、STOPPED：
   *   返回 false，不修改当前 Publisher。
   *
   * 空 AlarmPublisher 表示注销。
   * @param publisher 可靠告警入队回调；空回调表示注销。
   * @return 操作成功时返回 true，否则返回 false。
   */
  bool set_alarm_publisher(AlarmPublisher publisher);
private:
  /** @brief use to control a,b,c and switch operating status */
  std::mutex _control_mutex;

  MonConfig _config;
  MonitorModuleRegistry _modules;
  std::unique_ptr<MonContext> _context;
  std::atomic<EnginePhase> _phase{EnginePhase::CREATED};
};

} // namespace TLSSMON

#endif // TLSSMON_ENGINE_H
