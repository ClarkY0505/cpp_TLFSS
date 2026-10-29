#ifndef __INC_COMMON_CACHE_ARC_CACHE_H__
#define __INC_COMMON_CACHE_ARC_CACHE_H__

#include "arc_lfu.h"
#include "arc_lru.h"
#include "cache_policy.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace TLSS::CACHE {
// LRU 接收近期数据，LFU 保存多次命中的热点；ghost 只记录淘汰历史。
// 同一个 key 只驻留在一个主缓存中，两个分区的容量配额之和为 cap。
template <typename Key, typename Value>
class ArcCache : public CachePolicy<Key, Value> {
 public:
  // 奇数容量的额外一个槽位留给 LRU；cap 为 1 时仅使用 LRU。
  // ghost 的预算使用总容量，避免分区缩到 0 后丢失后续反馈所需的历史。
  explicit ArcCache(size_t cap = 10, size_t trans = 2)
      : _capacity(cap)
      , _transform_threshold(trans)
      , _lru_capacity(cap - cap / 2)
      , _lru(std::make_unique<ArcLru<Key, Value>>(_lru_capacity, trans, cap))
      , _lfu(std::make_unique<ArcLfu<Key, Value>>(cap / 2, trans, cap)) {}
  ~ArcCache() override = default;

  void put(Key key, Value value) override {
    // 分片锁覆盖“查找、调整配额、写入”全过程，防止其他线程插入中间状态。
    std::lock_guard<std::mutex> lock(_mutex);
    if (_capacity == 0) {
      return;
    }
    // 已驻留的 key 只更新所属分区，不再次插入另一分区。
    if (_lru->contain(key)) {
      // 更新LRU部分缓存
      _lru->put(key, value);
      return;
    }
    // 检查LFU是否存在该键
    if (_lfu->contain(key)) {
      // 如果LFU部分存在该键，更新LFU
      _lfu->put(key, value);
      return;
    }

    // 只有真实数据重新装入时才消费 ghost，普通 get 未命中不改变容量配额。
    const auto hit = check_ghost_caches(key);
    // 曾从 LFU 淘汰的数据优先回到 LFU，避免重新经过 LRU 晋升流程。
    if (hit == GhostHit::Lfu && _lfu->put(key, value)) {
      return;
    }
    // 全新数据、LRU 历史命中及 LFU 无法接收的情况，都由 LRU 接收。
    _lru->put(key, value);
  }

  bool get(Key key, Value& value) override {
    // 读取只访问主缓存，避免先处理 ghost 时把本次要读的 LFU 节点淘汰。
    std::lock_guard<std::mutex> lock(_mutex);
    bool should_transform = false;
    if (_lru->get(key, value, should_transform)) {
      // LFU 接收成功后才从 LRU 移除，完成所有权转移且不产生 ghost。
      // 若 LFU 配额为 0，保留原 LRU 节点，本次读取仍然成功。
      if (should_transform && _lfu->put(key, value)) {
        _lru->erase(key);
      }
      return true;
    }
    // LRU 未命中再查 LFU；LFU 命中时由其内部维护访问频率。
    return _lfu->get(key, value);
  }

  Value get(Key key) override {
    Value value{};
    get(key, value);
    return value;
  }

 private:
  enum class GhostHit { None, Lru, Lfu };

  // 仅在主缓存均未命中的 put 中调用；调用者持有分片锁。
  GhostHit check_ghost_caches(const Key& key) {
    // 每次转移一个容量槽位：先收缩来源分区，再扩张目标分区，保持总量不变。
    // check_ghost 命中即删除该历史记录，避免一次重新装入被重复计算。
    // 命中 LRU Ghost：说明近期淘汰的数据被再次访问
    // 尝试减少 LFU 容量，增加 LRU 容量
    if (_lru->check_ghost(key)) {
      if (_lfu->decrease_capacity()) {
        _lru->increase_capacity();
        // 同步记录 LRU 配额，供反方向调整时检查最小入口容量。
        ++_lru_capacity;
      }
      return GhostHit::Lru;
    }
    // 命中 LFU Ghost：说明之前的热点数据被淘汰后再次访问
    // 尝试减少 LRU 容量，增加 LFU 容量
    if (_lfu->check_ghost(key)) {
      // 正容量缓存始终给新 key 保留一个 LRU 入口。
      if (_lru_capacity > 1 && _lru->decrease_capacity()) {
        _lfu->increase_capacity();
        --_lru_capacity;
      }
      return GhostHit::Lfu;
    }
    return GhostHit::None;
  }

  size_t _capacity;
  size_t _transform_threshold;
  size_t _lru_capacity;
  std::mutex _mutex;
  std::unique_ptr<ArcLru<Key, Value>> _lru;
  std::unique_ptr<ArcLfu<Key, Value>> _lfu;
};

template <typename Key, typename Value>
class HashArcCache {
 public:
  using CacheType = ArcCache<Key, Value>;
  using CachePtr = std::unique_ptr<CacheType>;

  // 分片数为 0 时按硬件并发度选择；同一个 key 始终映射到同一分片。
  // 每个分片独立加锁，分片之间可以并行访问。
  explicit HashArcCache(size_t cap, size_t slice_num)
      : _capacity(cap)
      , _slice_num(slice_num > 0 ? slice_num
                                 : std::max<std::size_t>(1, std::thread::hardware_concurrency())) {
    // 零容量表示禁用缓存，不创建分片，避免下面计算容量时除以 0。
    if (_capacity == 0) {
      _slice_num = 0;
      return;
    }
    // 避免分片数量超过容量，产生零容量分片
    _slice_num = std::min(_slice_num, _capacity);
    // 基础容量与剩余容量
    const std::size_t base_size = _capacity / _slice_num;
    const std::size_t remainder = _capacity % _slice_num;
    // 前 remainder 个分片多分一个槽位，使所有分片的容量之和恰好为 cap。

    _arc_slice_caches.reserve(_slice_num);
    /* const size_t slice_size = (cap / slice_num) + ((cap % slice_num) != 0 ? 1UL : 0UL); */
    for (size_t i = 0; i < _slice_num; ++i) {
      // 将剩余容量均匀分配给前面的分片
      const std::size_t slice_size = base_size + static_cast<std::size_t>(i < remainder);
      _arc_slice_caches.emplace_back(new CacheType(slice_size));
    }
  }

  void put(Key key, Value value) {
    if (_slice_num == 0) {
      return;
    }
    // 获取key的hash值，并计算出对应的分片索引
    size_t slice_idx = k_hash(key) % _slice_num;
    _arc_slice_caches[slice_idx]->put(key, value);
  }

  bool get(Key key, Value& value) {
    if (_slice_num == 0) {
      return false;
    }
    // 获取key的hash值，并计算出对应的分片索引
    size_t slice_idx = k_hash(key) % _slice_num;
    return _arc_slice_caches[slice_idx]->get(key, value);
  }

  Value get(Key key) {
    Value value{};
    /* UTIL::memzero(&value, sizeof value); */
    get(key, value);
    return value;
  }

 private:
  size_t k_hash(Key key) {
    std::hash<Key> hash_func;
    return hash_func(key);
  }

  size_t _capacity;
  size_t _slice_num;
  std::vector<CachePtr> _arc_slice_caches;
};
}  // namespace TLSS::CACHE

#endif  // __INC_COMMON_CACHE_ARC_CACHE_H__
