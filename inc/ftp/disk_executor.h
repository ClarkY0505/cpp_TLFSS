#ifndef TLSS_FTP_DISK_EXECUTOR_H
#define TLSS_FTP_DISK_EXECUTOR_H
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "common/NoCopy.h"
#include "common/base/tlss_thread_pool.h"

namespace TLSS::FileStorage::Transfer {
class AsyncReceiver;
class AsyncFileSource;
class DiskExecutor : NoCopy {
  public:
    static constexpr std::size_t kMaxReceivers = 64;
    explicit DiskExecutor(const std::string& name = "FileDiskPool");
    ~DiskExecutor();

    void start(int threads = 2);
    void begin_shutdown();
    void stop();

  private:
    friend class AsyncReceiver;
    friend class AsyncFileSource;

    enum class Phase { created, running, closing, stopped };

    template <typename State>
    void attach(const std::shared_ptr<State>& state) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_phase != Phase::running) {
            throw std::runtime_error("disk executor is not accepting transfers");
        }

        if (_receivers.size() >= kMaxReceivers) {
            throw std::runtime_error("too many active file transfers");
        }

        const std::uint64_t token = next_token.fetch_add(1, std::memory_order_relaxed);
        if (token == 0) {
            throw std::runtime_error("file transfer token space exhausted");
        }

        // 在发布取消 hook 之前设置 token。
        state->token = token;
        std::function<void()> cancel_cb = [weak = std::weak_ptr<State>(state)] {
            if (auto locked_state = weak.lock()) {
                locked_state->cancel();
            }
        };

        _receivers.emplace(token, cancel_cb);
    }
    bool post(BASE::ThreadPool::Task task, bool cleanup);
    void release(std::uint64_t token);

    BASE::ThreadPool _pool;

    std::mutex _lifecycle_mutex;
    std::mutex _mutex;
    std::condition_variable _drained;

    Phase _phase = Phase::created;
    std::unordered_map<std::uint64_t, std::function<void()>> _receivers;
    inline static std::atomic<std::uint64_t> next_token{1};
};

}  // namespace TLSS::FileStorage::Transfer
#endif
