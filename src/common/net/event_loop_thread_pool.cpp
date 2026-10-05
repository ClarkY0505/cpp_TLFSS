#include "common/net/event_loop_thread_pool.h"
#include "common/net/event_loop.h"
#include "common/net/event_loop_thread.h"
#include <cstdio>
#include <memory>
#include <vector>

namespace TLSS::NET {
EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, const std::string& name_arg)
    : _base_loop(base_loop)
    , _name(name_arg)
    , _started(false)
    , _num_thread(0)
    , _next(0) {}

// std::vector<EventLoop*> _loops
// 容器中的对象，只工作在线程中
// 所以在这里不需要析构
EventLoopThreadPool::~EventLoopThreadPool() {}

void EventLoopThreadPool::start(const ThreadInitCallback &cb){
    _started = true;
    for(int i = 0 ; i < _num_thread; ++i){
        //
        // 这个方法仍然适用
        // 但是在C++17中不支持变长数组
        // 如果使用c++17会编译失败
        /* char buffer[_name.size() + 32]; */
        /* snprintf(buffer, sizeof buffer, "%s%d", _name.c_str(), i); */
        /* EventLoopThread *elt = new EventLoopThread(cb,buffer); */
        const std::string thread_name = _name + std::to_string(i);
        EventLoopThread *elt = new EventLoopThread(cb,thread_name);
        _threads.push_back(std::unique_ptr<EventLoopThread>(elt));
        //
        // 绑定一个新的event loop并返回event loop的地址
        _loops.push_back(elt->start_loop());
    }

    // 只有一个线程在运行
    // main loop
    if(_num_thread == 0 && cb){
        cb(_base_loop);
    }
}

//
// 就是一个简单的轮询
// 如果工作在多线程中， _base_loop默认以轮询的方式去分配channel给sub loop
EventLoop* EventLoopThreadPool::get_next_loop(){
    //
    // main loop
    EventLoop* loop = _base_loop;
    if(!_loops.empty()){
        loop = _loops[_next];
        ++_next;
        if(_next >= _loops.size()){
            _next = 0;
        }
    }

    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::get_all_loops(){
    if(_loops.empty()){
        return std::vector<EventLoop*>(1,_base_loop);
    }
    else{
        return _loops;
    }
}

}  // namespace TLSS::NET
