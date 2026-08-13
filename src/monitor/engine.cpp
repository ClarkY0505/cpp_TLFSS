#include "engine.h"
#include "aio_manager.h"
#include "callback_registry.h"
#include "timer_manager.h"
#include "wake_pipe.h"


#include <atomic>
#include <bits/types/struct_timeval.h>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <sys/select.h>
#include <utility>
namespace TLSSMON {

namespace EngineUtil{
constexpr bool accepts_registration(EnginePhase phase) noexcept
{
    return phase == EnginePhase::READY || phase == EnginePhase::RUNNING;
}

}

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
        _aio(_cbs,_wakeup),
        _timers(_cbs,_wakeup)
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
    TimerManager _timers;

    std::optional<AioHandle> _wakeup_handle;
};

Engine::Engine(MonConfig config):_config(std::move(config)){}
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

    try{
        auto context = std::make_unique<MonContext>(_config);
        const PIPESTATUS pipe_status = context->_wakeup.init();
        if (PIPESTATUS::SUCCESSFUL != pipe_status) {
            _phase.store(EnginePhase::CREATED, std::memory_order_release);
            return ENGINESTATE::PIPEINITERR;
        }

        MonContext* context_ptr = context.get();
        MonCallback wakeup_cb{
            "wakeup", 
                [context_ptr]() noexcept -> int {
                    const PIPESTATUS status = context_ptr->_wakeup.drain();
                    return static_cast<int>(status);
                },
                false
        };

        auto wakeup_handle = context->_aio.add(context->_wakeup.read_fd(),
                                               std::move(wakeup_cb));
        if(!wakeup_handle){
            _phase.store(
                         EnginePhase::CREATED,
                         std::memory_order_release);

            return ENGINESTATE::AIOINITERR;
        }
        context->_wakeup_handle = *wakeup_handle;

        _context = std::move(context);
        _phase.store(EnginePhase::READY, std::memory_order_release);

        return ENGINESTATE::SUCCESSFUL;

    } catch(const std::bad_alloc&){
        _phase.store(EnginePhase::CREATED, std::memory_order_release);

        return ENGINESTATE::INITFAILED;
    }


}

ENGINESTATE Engine::run(){
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

    MonContext* const context = _context.get();
    if (nullptr == context){
        _phase.store(EnginePhase::STOPPED,  std::memory_order_release);
        return ENGINESTATE::NOTREADY;
    }

    const int wakeup_fd = context->_wakeup.read_fd();
    if(wakeup_fd < 0 || wakeup_fd >= FD_SETSIZE){
        _phase.store(EnginePhase::STOPPED, std::memory_order_release);
        return ENGINESTATE::PIPEERROR;
    }

    lock.unlock();

    ENGINESTATE result = ENGINESTATE::SUCCESSFUL;

    while(_phase.load(std::memory_order_acquire) == EnginePhase::RUNNING){
        timeval timeout{};
        context->_timers.check(timeout);
        if(context->_aio.process(&timeout) < 0){
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

void Engine::stop(){
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
        if(wakeup_status != PIPESTATUS::SUCCESSFUL){
            // TODO Log
            (void)wakeup_status;
        }
    }

}

EnginePhase Engine::get_phase() const noexcept{
    return _phase.load(std::memory_order_acquire);
}

std::optional<AioHandle> Engine::add_aio(int fd, MonCallback cb){
    std::lock_guard<std::mutex> lock(_control_mutex);
    const EnginePhase phase = _phase.load(std::memory_order_acquire);

    if(!EngineUtil::accepts_registration(phase)){
        return std::nullopt;
    }

    if(_context == nullptr){
        return std::nullopt;
    }

    // WakeupPipe is  internal contorl fd
    if(fd == _context->_wakeup.read_fd()){
        return std::nullopt;
    }

    return _context->_aio.add(fd, std::move(cb));
}

bool Engine::remove_aio(AioHandle handle){
    if(!handle){
        return false;
    }

    std::lock_guard<std::mutex> lock(_control_mutex);

    const EnginePhase phase = _phase.load(std::memory_order_acquire);
    if(!EngineUtil::accepts_registration(phase)){
        return false;
    }

    if (_context == nullptr) {
        return false;
    }

    if(_context->_wakeup_handle && _context->_wakeup_handle->_id == handle._id){
        return false;
    }

    return _context->_aio.remove(handle);
}

std::optional<TimerHandle> Engine::set_timer(MonCallback mcb, TimerFlags flags, std::chrono::milliseconds delay){
    std::lock_guard<std::mutex> lock(_control_mutex);
    const EnginePhase phase = _phase.load(std::memory_order_acquire);

    if(!EngineUtil::accepts_registration(phase)){
        return std::nullopt;
    }

    if(_context == nullptr){
        return std::nullopt;
    }

    return _context->_timers.add(std::move(mcb), flags, delay);
}

} // namespace TLSSMON
