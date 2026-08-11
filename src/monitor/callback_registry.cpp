#include "callback_registry.h"
#include "engine_type.h"

#include <chrono>
#include <algorithm>
#include <iostream>
#include <cstdint>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace TLSSMON {
EnhancedCallback::EnhancedCallback(MonCallback cb) : _mcb(std::move(cb)){}

EnhancedCallback::~EnhancedCallback(){
    stop_worker();
}

const std::string& EnhancedCallback::name() const noexcept{
    return _mcb._name;
}

CallbackStats EnhancedCallback::stats() const{
    std::lock_guard<std::mutex> lock(_stats_mutex);
    return _stats;
}

int EnhancedCallback::invoke_and_record() noexcept{
    const auto start = std::chrono::steady_clock::now();
    int res = 0;
    try{
        if (_mcb._cb){
            res = _mcb._cb();
        }
    } catch (...){
        // TODO Log
        res = -1;
    }
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(finish - start);
    const auto elapsed_us = static_cast<std::uint64_t>(elapsed.count());

    {
        std::lock_guard<std::mutex> lock(_stats_mutex);

        ++_stats._count;
        _stats._total_us += elapsed_us;

        if (elapsed_us < _stats._min_us){
            _stats._min_us = elapsed_us;
        }

        if(elapsed_us > _stats._max_us){
            _stats._max_us = elapsed_us;
        }
    }

    return res;
}

int EnhancedCallback::activate(){
    if(!_mcb._asynchronous){
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if(_stopping){
                return -1;
            }
        }

        return invoke_and_record();
    }

    std::unique_lock<std::mutex> lock(_mutex);
    if(_stopping){
        return -1;
    }

    if(!_worker_running){
        _worker_running = true;

        try{
            _worker = std::thread(&EnhancedCallback::worker_loop,this);
        } catch (const std::system_error& e) {
            _worker_running = false;

            // 日志：e.what()
            // 错误码：e.code().value()
            return -1;
        } catch (const std::bad_alloc& e) {
            //std::system_error：系统无法创建线程，例如线程数量达到上限、内存或系统资源不足
            _worker_running = false;

            // 日志：内存分配失败
            return -1;
        } catch (...) {
            // std::bad_alloc：创建线程相关对象时内存分配失败。
            _worker_running = false;

            // 日志：未知异常
            return -1;
        }
    }
    _pending = true;

    lock.unlock();
    _condition.notify_one();

    return 0;
}

void EnhancedCallback::worker_loop() noexcept{
    std::unique_lock<std::mutex> lock(_mutex);

    while(!_stopping){
        _condition.wait(lock,[this]{
                        return _stopping || _pending;
                        });

        if(_stopping){
            break;
        }

        _pending = false;

        lock.unlock();
        invoke_and_record();
        lock.lock();
    }

    _worker_running = false;
}

void EnhancedCallback::stop_worker(){
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if(_stopping){
            return;
        }

        _stopping = true;
        _pending = false;
    }

    _condition.notify_one();
    if(_worker.joinable()){
        _worker.join();
    }
}

bool EnhancedCallback::asynchronous() const noexcept
{
    return _mcb._asynchronous;
}

void CallbackRegistry::add(EnhancedCallback* cb){
    if(nullptr == cb){
        // TODO Log
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    // Registry只是一个语义集合，保证同一个回调不能重复注册
    const auto pos = std::find(_cbs.begin(),_cbs.end(),cb);

    if(_cbs.end() == pos){
        _cbs.push_back(cb);
    }
}

void CallbackRegistry::remove(EnhancedCallback* cb){
    if(nullptr == cb){
        // TODO Log
        return ;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    _cbs.erase(std::remove(_cbs.begin(),_cbs.end(),cb),_cbs.end());
}

void CallbackRegistry::stop_workers(){
    std::vector<EnhancedCallback*> callbacks;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        callbacks = _cbs;
    }

    for(EnhancedCallback * cb : callbacks){
        if(nullptr != cb && cb->asynchronous()){
            cb->stop_worker();
        }
    }
}

void CallbackRegistry::print_stats() const {
    std::vector<EnhancedCallback*> callbacks;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        callbacks = _cbs;
    }

    std::clog << "[TLSSmon] callback stats\n";

    for(const EnhancedCallback * cb : callbacks){
        if(nullptr == cb){
            continue;
        }

        const CallbackStats stats = cb->stats();
        if(0 == stats._count){
            std::clog << "[TLSSmon] callback=" << cb->name() << "never-activated\n";
            continue;
        }

        const std::uint64_t average_us = stats._total_us / stats._count;

        std::clog << "[TLSSmon] callbacks=" << cb->name() << " count=" << stats._count 
                  << " min_us = " << stats._min_us 
                  << " avg_us = " << average_us
                  << " max_us = " << stats._max_us << '\n';
    }
}

}
