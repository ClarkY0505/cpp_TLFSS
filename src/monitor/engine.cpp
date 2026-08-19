#include "aio_manager.h"
#include "callback_registry.h"
#include "engine.h"
#include "engine_type.h"
#include "monitor_data.h"
#include "monitor_store.h"
#include "timer_manager.h"
#include "wake_pipe.h"
#include "monitor_reporter.h"

#include <algorithm>
#include <atomic>
#include <bits/types/struct_timeval.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <sys/select.h>
#include <utility>
namespace TLSSMON {

namespace EngineUtil {
/**
 * 判断当前 Engine 状态是否接受监控数据写入。
 *
 * 只有 READY 和 RUNNING 接受写入。
 */
constexpr bool accepts_data_write(EnginePhase phase) noexcept {
    return phase == EnginePhase::READY || phase == EnginePhase::RUNNING;
}

constexpr bool accepts_registration(EnginePhase phase) noexcept {
    return phase == EnginePhase::READY || phase == EnginePhase::RUNNING;
}

/**
 * 判断当前 Engine 状态是否允许读取监控数据。
 *
 * STOPPING 和 STOPPED 时 MonContext 仍然存在，
 * 因此最后保存的监控数据仍然可以读取。
 */
constexpr bool accepts_data_read(EnginePhase phase) noexcept {
    return phase == EnginePhase::READY || phase == EnginePhase::RUNNING ||
        phase == EnginePhase::STOPPING || phase == EnginePhase::STOPPED;
}

static_assert(!accepts_data_write(EnginePhase::CREATED));

static_assert(!accepts_data_write(EnginePhase::INITIALIZING));

static_assert(accepts_data_write(EnginePhase::READY));

static_assert(accepts_data_write(EnginePhase::RUNNING));

static_assert(!accepts_data_write(EnginePhase::STOPPING));

static_assert(!accepts_data_write(EnginePhase::STOPPED));

static_assert(!accepts_data_read(EnginePhase::CREATED));

static_assert(!accepts_data_read(EnginePhase::INITIALIZING));

static_assert(accepts_data_read(EnginePhase::READY));

static_assert(accepts_data_read(EnginePhase::RUNNING));

static_assert(accepts_data_read(EnginePhase::STOPPING));

static_assert(accepts_data_read(EnginePhase::STOPPED));

} // namespace EngineUtil

struct MonContext final {
    /**
     * @brief 根据监控配置创建运行上下文，并建立 AIO 管理器与回调注册表、
     *        唤醒管道之间的关联。
     *        Creates the runtime context from the monitoring configuration and
     *        connects the AIO manager with the callback registry and wakeup pipe.
     *
     * @param config[in] 监控配置。配置内容会被移动到上下文中。
     *               Monitoring configuration whose contents are moved into the
     * context.
     */
    explicit MonContext(MonConfig config)
        : _name(std::move(config._name)), 
        _cli_port(config._port),
        _software_id(config._id),
        _reporter(_store),
        _aio(_cbs, _wakeup), 
        _timers(_cbs, _wakeup) {}

    MonContext(const MonContext &) = delete;
    MonContext &operator=(const MonContext &) = delete;

    const std::string _name;
    const std::uint16_t _cli_port;
    const std::uint8_t _software_id;

    MonitorStore _store;
    MonitorReporter _reporter;

    CallbackRegistry _cbs;
    WakeupPipe _wakeup;
    AioManager _aio;
    TimerManager _timers;

    std::optional<AioHandle> _wakeup_handle;
};

Engine::Engine(MonConfig config) : _config(std::move(config)) {}
Engine::~Engine() = default;

ENGINESTATE Engine::init() {

    EnginePhase expected = EnginePhase::CREATED;
    if (!_phase.compare_exchange_strong(expected, EnginePhase::INITIALIZING,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        return ENGINESTATE::ALREADYINITIALIZED;
    }

    if (_config._name.empty()) {
        _phase.store(EnginePhase::CREATED, std::memory_order_release);

        return ENGINESTATE::INVALIDCONFIG;
    }

    try {
        auto context = std::make_unique<MonContext>(_config);
        const PIPESTATUS pipe_status = context->_wakeup.init();
        if (PIPESTATUS::SUCCESSFUL != pipe_status) {
            _phase.store(EnginePhase::CREATED, std::memory_order_release);
            return ENGINESTATE::PIPEINITERR;
        }

        MonContext *context_ptr = context.get();
        MonCallback wakeup_cb{"wakeup",
            [context_ptr]() noexcept -> int {
                const PIPESTATUS status =
                    context_ptr->_wakeup.drain();
                return static_cast<int>(status);
            },
            false};

        auto wakeup_handle =
            context->_aio.add(context->_wakeup.read_fd(), std::move(wakeup_cb));
        if (!wakeup_handle) {
            _phase.store(EnginePhase::CREATED, std::memory_order_release);

            return ENGINESTATE::AIOINITERR;
        }
        context->_wakeup_handle = *wakeup_handle;

        _context = std::move(context);
        _phase.store(EnginePhase::READY, std::memory_order_release);

        return ENGINESTATE::SUCCESSFUL;

    } catch (const std::bad_alloc &) {
        _phase.store(EnginePhase::CREATED, std::memory_order_release);

        return ENGINESTATE::INITFAILED;
    }
}

ENGINESTATE Engine::run() {
    std::unique_lock<std::mutex> lock(_control_mutex);
    EnginePhase expected = EnginePhase::READY;

    if (!_phase.compare_exchange_strong(expected, EnginePhase::RUNNING,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        if (expected == EnginePhase::RUNNING) {
            return ENGINESTATE::ALREADYRUNNING;
        }

        return ENGINESTATE::NOTREADY;
    }

    MonContext *const context = _context.get();
    if (nullptr == context) {
        _phase.store(EnginePhase::STOPPED, std::memory_order_release);
        return ENGINESTATE::NOTREADY;
    }

    const int wakeup_fd = context->_wakeup.read_fd();
    if (wakeup_fd < 0 || wakeup_fd >= FD_SETSIZE) {
        _phase.store(EnginePhase::STOPPED, std::memory_order_release);
        return ENGINESTATE::PIPEERROR;
    }

    lock.unlock();

    ENGINESTATE result = ENGINESTATE::SUCCESSFUL;

    while (_phase.load(std::memory_order_acquire) == EnginePhase::RUNNING) {
        timeval timeout{};
        context->_timers.check(timeout);
        if (context->_aio.process(&timeout) < 0) {
            result = ENGINESTATE::WAITFAILED;
            break;
        }
    }

    lock.lock();
    if (_phase.load(std::memory_order_acquire) == EnginePhase::RUNNING) {
        _phase.store(EnginePhase::STOPPING, std::memory_order_release);
    }
    lock.unlock();
    context->_cbs.stop_workers();
    context->_cbs.print_stats();
    context->_timers.cleanup();
    context->_aio.cleanup();

    lock.lock();
    _context->_wakeup.pipe_close();
    _phase.store(EnginePhase::STOPPED, std::memory_order_release);

    return result;
}

void Engine::stop() {
    {
        std::lock_guard<std::mutex> lock(_control_mutex);
        EnginePhase expected = EnginePhase::RUNNING;

        if (!_phase.compare_exchange_strong(expected, EnginePhase::STOPPING,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return;
        }

        if (_context == nullptr) {
            return;
        }
        // status is STOPPING , now new add/remove will be rejected
        const PIPESTATUS wakeup_status = _context->_wakeup.wakeup();
        if (wakeup_status != PIPESTATUS::SUCCESSFUL) {
            // TODO Log
            (void)wakeup_status;
        }
    }
}

EnginePhase Engine::get_phase() const noexcept {
    return _phase.load(std::memory_order_acquire);
}

std::optional<AioHandle> Engine::add_aio(int fd, MonCallback cb) {
    std::lock_guard<std::mutex> lock(_control_mutex);
    const EnginePhase phase = _phase.load(std::memory_order_acquire);

    if (!EngineUtil::accepts_registration(phase)) {
        return std::nullopt;
    }

    if (_context == nullptr) {
        return std::nullopt;
    }

    // WakeupPipe is  internal contorl fd
    if (fd == _context->_wakeup.read_fd()) {
        return std::nullopt;
    }

    return _context->_aio.add(fd, std::move(cb));
}

bool Engine::remove_aio(AioHandle handle) {
    if (!handle) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_control_mutex);

    const EnginePhase phase = _phase.load(std::memory_order_acquire);
    if (!EngineUtil::accepts_registration(phase)) {
        return false;
    }

    if (_context == nullptr) {
        return false;
    }

    if (_context->_wakeup_handle && _context->_wakeup_handle->_id == handle._id) {
        return false;
    }

    return _context->_aio.remove(handle);
}

std::optional<TimerHandle> Engine::set_timer(MonCallback mcb, TimerFlags flags,
                                             std::chrono::milliseconds delay) {
    std::lock_guard<std::mutex> lock(_control_mutex);
    const EnginePhase phase = _phase.load(std::memory_order_acquire);

    if (!EngineUtil::accepts_registration(phase)) {
        return std::nullopt;
    }

    if (_context == nullptr) {
        return std::nullopt;
    }

    return _context->_timers.add(std::move(mcb), flags, delay);
}


/*
 * 下例三函数为什么不使用std::lock_guard<std::mutex> lock(_control_mutex);
 * - MonitorStore 已通过自己的 _mutex 保证线程安全。
 * - 多个 Timer/AIO worker 应能并发进入 update_data()。
 * - 使用 _control_mutex 包围整个 Store 操作，会在 Engine 层把所有写入和查询额外串行化。
 * - _context 在 init() 成功后发布，停止时不会被清空或替换。
 * */
MonData::UpdateResult Engine::update_data(MonData::MonitorData data, bool force){
    /*
     * acquire 与 init() 发布 READY 时的 release 配对。
     *
     * 如果当前线程读取到 READY 或后续状态，
     * _context 的初始化结果已经对当前线程可见。
     */
    const EnginePhase phase =
        _phase.load(std::memory_order_acquire);

    if (!EngineUtil::accepts_data_write(phase)) {
        return MonData::UpdateResult{
            MonData::UpdateStatus::INVALID,
                std::nullopt
        };
    }

    MonContext* const context = _context.get();
    if(context == nullptr){
        return MonData::UpdateResult{MonData::UpdateStatus::INVALID, std::nullopt};
    }

    /*
     * Engine 公共接口不要求调用者传时间戳。
     *
     * system_clock 表示记录实际发生时间；
     * Timer deadline 使用的 steady_clock 不能用于这里。
     */
    const MonData::MonitorTimestamp timestamp = std::chrono::system_clock::now();

    /*
     * MonitorData 按值进入接口，因此可以安全移动给 Store。
     */
    return context->_reporter.update(std::move(data), force, timestamp);
}

std::optional<MonData::StoredRecord> Engine::find_data(const MonData::MonitorKey& key) const{
    const EnginePhase phase = _phase.load(std::memory_order_acquire);
    /*
     * CREATED、INITIALIZING 没有可读取的 Store。
     *
     * READY、RUNNING、STOPPING、STOPPED 都允许读取。
     */
    if (!EngineUtil::accepts_data_read(phase)) {
        return std::nullopt;
    }

    const MonContext* const context = _context.get();

    if (context == nullptr) {
        return std::nullopt;
    }

    /*
     * MonitorStore::find() 返回 StoredRecord 副本，
     * 不暴露 Store 内部引用。
     */
    return context->_store.find(key);
}

std::vector<MonData::StoredRecord> Engine::query_data(const MonData::MonitorFilter& filter) const{
    const EnginePhase phase = _phase.load(std::memory_order_acquire);
    if(!EngineUtil::accepts_data_read(phase)){
        return {};
    }

    const MonContext* const context = _context.get();
    if(context == nullptr){
        return {};
    }

    /*
     * MonitorStore::query() 返回排序后的独立快照。
     */
    return context->_store.query(filter);
}

bool Engine::set_publisher(MonitorPublisher publisher) {
    std::lock_guard<std::mutex> lock(_control_mutex);
    const EnginePhase phase = _phase.load(std::memory_order_acquire);
    if (!EngineUtil::accepts_data_write(phase)) {
          return false;
      }

    if (_context == nullptr) {
          return false;
      }

    _context->_reporter.set_publisher(std::move(publisher));
    return true;
}

MonData::UpdateResult Engine::report_count(MonData::MonitorKey key, std::uint32_t value, std::string description){
    const EnginePhase phase = _phase.load(std::memory_order_acquire);
    if (!EngineUtil::accepts_data_write(phase)) {
          return {
              MonData::UpdateStatus::INVALID,
              std::nullopt
          };
      }

    MonContext* const context = _context.get();
    if(context == nullptr){
        return {MonData::UpdateStatus::INVALID, std::nullopt};
    }

    const MonData::MonitorTimestamp timestamp = std::chrono::system_clock::now();
    return context->_reporter.report_count(std::move(key), value, std::move(description), timestamp);
}

MonData::UpdateResult Engine::report_error(MonData::MonitorKey key, std::uint32_t value, std::string description){
    const EnginePhase phase = _phase.load(std::memory_order_acquire);
    if (!EngineUtil::accepts_data_write(phase)) {
          return {
              MonData::UpdateStatus::INVALID,
              std::nullopt
          };
      }

    MonContext* const context = _context.get();
    if(context == nullptr){
        return {MonData::UpdateStatus::INVALID, std::nullopt};
    }

    const MonData::MonitorTimestamp timestamp = std::chrono::system_clock::now();
    return context->_reporter.report_error(std::move(key), value, std::move(description), timestamp);
}

MonData::UpdateResult Engine::report_string(MonData::MonitorKey key, std::string value, std::string description){
    const EnginePhase phase = _phase.load(std::memory_order_acquire);
    if (!EngineUtil::accepts_data_write(phase)) {
          return {
              MonData::UpdateStatus::INVALID,
              std::nullopt
          };
      }

    MonContext* const context = _context.get();
    if(context == nullptr){
        return {MonData::UpdateStatus::INVALID, std::nullopt};
    }

    const MonData::MonitorTimestamp timestamp = std::chrono::system_clock::now();
    return context->_reporter.report_string(std::move(key), std::move(value), std::move(description), timestamp);
}
} // namespace TLSSMON
