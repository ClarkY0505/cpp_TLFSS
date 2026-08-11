#include "aio_manager.h"

#include "aio_types.h"
#include "callback_registry.h"
#include "wake_pipe.h"
#include "engine_type.h"

#include <algorithm>
#include <cerrno>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sys/select.h>
#include <utility>
#include <vector>

namespace TLSSMON {

struct AioEntry {
    std::uint64_t _id{0};
    int _fd{-1};
    bool _pending_remove{false};
    std::unique_ptr<EnhancedCallback> _cb;
};

AioManager::AioManager(CallbackRegistry& registry, WakeupPipe& wakeup)
    : _registry(registry),
    _wakeup(wakeup)
{
}

AioManager::~AioManager(){
    cleanup();
}

void AioManager::cleanup(){
    std::list<std::unique_ptr<AioEntry>> entries;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if(_cleaned_up){
            return;
        }

        _cleaned_up = true;
        entries.splice(entries.end(), _entries);
    }


    for(auto & e : entries){
        if(e && e->_cb){
            _registry.remove(e->_cb.get());
        }
    }

    entries.clear();
}

std::optional<AioHandle> AioManager::add(int fd, MonCallback cb){
    if(fd < 0 || fd >= FD_SETSIZE){
        return std::nullopt;
    }

    if(PipeState::READY != _wakeup.state()){
        return std::nullopt;
    }

    if(!cb._cb){
        return std::nullopt;
    }

    try{

        auto entry = std::make_unique<AioEntry>();
        entry->_fd = fd;
        entry->_pending_remove = false;
        entry->_cb = std::make_unique<EnhancedCallback>(std::move(cb));

        std::lock_guard<std::mutex> lock(_mutex);

        if(_cleaned_up){
            return std::nullopt;
        }

        if(_next_id == 0){
            // uint64_t 回绕，0 被保留为无效 handle。
            return std::nullopt;
        }

        const AioHandle handle{_next_id};
        entry->_id = handle._id;
        EnhancedCallback* cb = entry->_cb.get();
        // 先让 AioManager 接管回调所有权。
        _entries.push_front(std::move(entry));

        try{
            // Registry 只保存非拥有指针。
            _registry.add(cb);
        } catch(const std::bad_alloc&){
            _entries.pop_front();
            return std::nullopt;
        }

        const PIPESTATUS pipe_wakeup_status = _wakeup.wakeup();
        if(PIPESTATUS::SUCCESSFUL != pipe_wakeup_status){
            _registry.remove(cb);
            _entries.pop_front();
            return std::nullopt;
        }

        ++_next_id;
        return handle;
    } catch(const std::bad_alloc&){
        return std::nullopt;
    }
}

bool AioManager::remove(AioHandle handle){
    if(!handle){
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if(_cleaned_up){
        return false;
    }

    // auto& entry is std::unique_ptr<AioEntry> entry
    const auto pos = std::find_if(_entries.begin(),_entries.end(),
                                  [handle](const auto& entry){
                                  return entry && entry->_id == handle._id;
                                  });

    if(pos == _entries.end() || (*pos)->_pending_remove){
        return false;
    }

    (*pos)->_pending_remove = true;

    if(_wakeup.wakeup() != PIPESTATUS::SUCCESSFUL){
        (*pos)->_pending_remove = false;
        return false;
    }

    return true;
}

int AioManager::process(timeval* timeout){
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_cleaned_up) {
            return 0;
        }
    }

    std::list<std::unique_ptr<AioEntry>> retired;
    /*
     * 创建armed快照
     * 原始流程 根据当时的 _entries 构造 fd_set -> select() -> 再次遍历_entries
     * 如果在select()阻塞期间另一个线程添加了AIO,第二次遍历会看到一个并未参与本轮select()的新节点
     * 特别是新节点与已有节点使用相同fd时,此时回调可能被错误激活
     *
     * 因此，select() 返回后，只遍历 armed_entries，不要遍历最新的 _entries
     * 当前预期生命周期为
     * 构造 armed_entries -> select() -> 检查 armed_entries -> 执行回调 
     * -> process() 返回 -> 下一轮 process() 才回收 pending 节点
     * 但是这个方法不确定是否可以正确的达到预期
     * TODO
     * */
    std::vector<AioEntry*> armed_entries;
    int max_fd = -1;

    fd_set read_fds;
    FD_ZERO(&read_fds);

    {
        std::lock_guard<std::mutex> lock(_mutex);

        /*
         * 必须在移动 retired 节点之前 reserve。
         *
         * 如果 reserve 抛出 bad_alloc，此时 _entries 尚未变化，
         * 不会出现节点已释放但 Registry 尚未注销的问题。
         */
        armed_entries.resize(_entries.size());
        for(auto it = _entries.begin(); it != _entries.end(); ){
            AioEntry* const entry = it->get();
            if(entry == nullptr){
                auto current = it++;
                retired.splice(retired.end(), _entries, current);
                continue;
            }

            if(entry->_pending_remove){
                auto current = it++;
                retired.splice(retired.end(), _entries, current);
                continue;
            }

            FD_SET(entry->_fd, &read_fds);
            max_fd = std::max(max_fd, (*it)->_fd);

            // 保存本轮真正加入fd_set的节点
            armed_entries.push_back(entry);
            ++it;
        }
    }

    for(auto& entry : retired){
        if(entry && entry->_cb){
            _registry.remove(entry->_cb.get());
        }
    }

    retired.clear();

    const int ready_count = ::select(max_fd + 1, &read_fds, nullptr,nullptr, timeout);
    if(ready_count < 0){
        if(EINTR == errno){
            return 0;
        }

        return -1;
    }

    if(ready_count == 0){
        return 0;
    }

    std::vector<EnhancedCallback*> ready_cbs;
    try {
        /*
         * 最多一个 armed entry 产生一个 ready callback。
         * 提前 reserve，保证锁内 push_back 不再分配内存。
         */
        ready_cbs.reserve(armed_entries.size());
    } catch (const std::bad_alloc&) {
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for(AioEntry* const  entry : armed_entries){
            if(!entry || entry->_pending_remove || !entry->_cb){
                continue;
            }

            if(FD_ISSET(entry->_fd, &read_fds)){
                ready_cbs.push_back(entry->_cb.get());
            }
        }
    }

    /*
     * 用户回调必须在 AioManager::_mutex 之外执行，
     * 允许回调内部调用 add()、remove() 或 Engine::stop()。
     */
    for(EnhancedCallback* cb : ready_cbs){
        if (cb != nullptr){
            (void)cb->activate();
        }
    }

    return 0;
}

} // namespace TLSSMON
