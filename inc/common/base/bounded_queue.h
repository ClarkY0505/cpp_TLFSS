#ifndef __INC_COMMON_BASE_BOUNDED_QUEUE_H__
#define __INC_COMMON_BASE_BOUNDED_QUEUE_H__

#include <dirent.h>
#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#include "common/NoCopy.h"

namespace TLSS::BASE {

template <typename T>
class BoundeQueue : public NoCopy {
 public:
  enum class PushResult { success, full, closed };
  //
  // 后续使用unique_ptr或shared_ptr,满足这个条件
  // 保证取出元素时，移动操作不会中途抛出异常
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "T must support noexcept move construction");

  explicit BoundeQueue(std::size_t capacity)
      : _capacity(capacity) {
    if (_capacity == 0) {
      throw std::invalid_argument("BoundedQueue capacity must be greater than zero");
    }
  }

  //
  // 成功时移动 value
  // 失败时 value保持原来的所有权
  PushResult try_push(T&& value) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_closed) {
      return PushResult::closed;
    }

    if (_queue.size() >= _capacity) {
      return PushResult::full;
    }

    _queue.emplace_back(std::move(value));
    return PushResult::success;
  }

  //
  // 按照FIFO顺序， 最多取出 max_count 个元素
  // 空队列时返回空vector
  std::vector<T> take_batch(std::size_t max_count) {
    std::vector<T> batch;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      const std::size_t count = std::min(max_count, _queue.size());

      // 先分配空间，在移动元素
      // 模板是静态多态，先分配好空间，在移动元素
      // 能够降低一定的开销
      batch.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        batch.emplace_back(std::move(_queue.front()));
        _queue.pop_front();
      }
    }
    return batch;
  }

  void close() {
    std::lock_guard<std::mutex> lock(_mutex);
    _closed = true;
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _queue.empty();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _queue.size();
  }

 private:
  const std::size_t _capacity;
  mutable std::mutex _mutex;
  std::deque<T> _queue;
  bool _closed = false;
};
}  // namespace TLSS::BASE
#endif  // __INC_COMMON_BASE_BOUNDED_QUEUE_H__
