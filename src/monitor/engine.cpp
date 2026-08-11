#include "engine.h"
#include "engine_type.h"
#include "wake_pipe.h"
#include <algorithm>
#include <atomic>
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
        /* fd_set read_fds; */
        /* FD_ZERO(&read_fds); */
        /* FD_SET(wakeup_fd, &read_fds); */

        /* const int ready_count = ::select(wakeup_fd + 1, &read_fds, nullptr, nullptr,nullptr); */

        /* if(ready_count < 0){ */
        /*     if(EINTR == errno){ */
        /*         continue; */
        /*     } */
        /*     result = ENGINESTATE::WAITFAILED; */
        /*     break; */
        /* } */

        /* if(ready_count > 0 && FD_ISSET(wakeup_fd, &read_fds)){ */
        /*     const PIPESTATUS pipe_status = context->_wakeup.drain(); */
        /*     if( PIPESTATUS::SUCCESSFUL != pipe_status){ */
        /*         result = ENGINESTATE::PIPEERROR; */
        /*         break; */
        /*     } */
        /* } */

        if(context->_aio.process(nullptr) < 0){
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
    context->_aio.cleanup();

    lock.lock();
    _phase.store(EnginePhase::STOPPED, std::memory_order_release);

    return result;
}

void Engine::stop(){
    WakeupPipe* wakeup = nullptr;
    {
        std::lock_guard<std::mutex> lock(_control_mutex);
        EnginePhase expected = EnginePhase::RUNNING;

        if (!_phase.compare_exchange_strong(expected, EnginePhase::STOPPING,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return;
        }

        if (_context != nullptr) {
            wakeup = &_context->_wakeup;
        }
    }

    // status is STOPPING , now new add/remove will be rejected
    if(wakeup != nullptr){
        (void)wakeup->wakeup();
    }
}

EnginePhase Engine::get_phase() const noexcept{
    return _phase.load(std::memory_order_acquire);
}

std::optional<AioHandle> Engine::add_aio(int fd, MonCallback cb){
    std::lock_guard<std::mutex> lock(_control_mutex);
    const EnginePhase phase = _phase.load(std::memory_order_acquire);

    if(phase != EnginePhase::READY && phase != EnginePhase::RUNNING){
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
    if(phase != EnginePhase::READY && phase != EnginePhase::RUNNING){
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

} // namespace TLSSMON
