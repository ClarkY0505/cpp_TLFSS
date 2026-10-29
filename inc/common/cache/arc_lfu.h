#ifndef __INC_COMMON_CACHE_ARC_LFU_H__
#define __INC_COMMON_CACHE_ARC_LFU_H__

#include "arc_node.h"

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace TLSS::CACHE {
// 按访问频率分桶；同一频率内，先进入桶的节点先被淘汰。
template <typename Key, typename Value>
class ArcLfu {
 public:
  using NodeType = ArcNode<Key, Value>;
  using NodePtr = std::shared_ptr<NodeType>;
  using NodeMap = std::unordered_map<Key, NodePtr>;
  // ghost 仅保留 key 和链表连接，避免历史记录继续占用 Value 的资源。
  using GhostNodeType = ArcNode<Key, std::nullptr_t>;
  using GhostNodePtr = std::shared_ptr<GhostNodeType>;
  using GhostMap = std::unordered_map<Key, GhostNodePtr>;
  using FreqMap = std::map<size_t, std::list<NodePtr>>;

  explicit ArcLfu(size_t cap, size_t trans)
      : ArcLfu(cap, trans, cap) {}

  ArcLfu(size_t cap, size_t trans, size_t ghost_cap)
      : _capacity(cap)
      , _ghost_capacity(ghost_cap)
      , _transform_threshold(trans)
      , _min_freq(0) {
    init_lists();
  }

  bool put(Key key, Value value) {
    // 容量调整与插入共用同一把锁，防止按过期配额插入节点。
    std::lock_guard<std::mutex> lock(_mutex);
    if (_capacity == 0) {
      return false;
    }
    auto it = _main_cache.find(key);
    if (it != _main_cache.end()) {
      return update_existing_node(it->second, value);
    }
    // 同 key 重新进入主缓存后，其旧淘汰记录不再参与反馈。
    erase_ghost(key);
    return add_new_node(key, value);
  }

  bool get(Key key, Value& value) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _main_cache.find(key);
    if (it != _main_cache.end()) {
      update_node_frequency(it->second);
      value = it->second->get_value();
      return true;
    }
    return false;
  }

  bool contain(Key key) {
    std::lock_guard<std::mutex> lock(_mutex);
    return _main_cache.find(key) != _main_cache.end();
  }

  // 命中后消费历史，由 ArcCache 决定是否为 LFU 增加配额。
  bool check_ghost(Key key) {
    std::lock_guard<std::mutex> lock(_mutex);
    return erase_ghost(key);
  }

  void increase_capacity() {
    std::lock_guard<std::mutex> lock(_mutex);
    ++_capacity;
  }

  bool decrease_capacity() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_capacity == 0) {
      return false;
    }
    // 先更新配额，再淘汰到符合新上限；这里允许 LFU 缩到 0。
    --_capacity;
    while (_main_cache.size() > _capacity) {
      evict_least_frequent();
    }
    return true;
  }

 private:
  // 调用方已持有 _mutex；移除链表节点与哈希索引必须在同一临界区完成。
  bool erase_ghost(const Key& key) {
    auto it = _ghost_cache.find(key);
    if (it == _ghost_cache.end()) {
      return false;
    }
    remove_from_ghost(it->second);
    _ghost_cache.erase(it);
    return true;
  }

  void init_lists() {
    _ghost_head = std::make_shared<GhostNodeType>();
    _ghost_tail = std::make_shared<GhostNodeType>();
    _ghost_head->_next = _ghost_tail;
    _ghost_tail->_prev = _ghost_head;
  }

  bool update_existing_node(NodePtr node, const Value& value) {
    // 覆盖已有值也计为一次访问，保持写入和读取对频率的更新规则一致。
    node->set_value(value);
    update_node_frequency(node);
    return true;
  }

  bool add_new_node(const Key& key, const Value& value) {
    // 装入前先腾出一个槽位，避免本分区超过当前容量配额。
    if (_main_cache.size() >= _capacity) {
      evict_least_frequent();
    }

    NodePtr new_node = std::make_shared<NodeType>(key, value);
    _main_cache[key] = new_node;

    // 将新节点添加到频率为1的列表中
    if (_freq_map.find(1) == _freq_map.end()) {
      _freq_map[1] = std::list<NodePtr>();
    }

    // 晋升及 ghost 重新装入的节点也从频率 1 开始，不继承 LRU 的访问次数。
    _freq_map[1].push_back(new_node);
    _min_freq = 1;
    return true;
  }

  void update_node_frequency(NodePtr node) {
    size_t old_freq = node->get_access_count();
    node->increment_access_count();
    size_t new_freq = node->get_access_count();

    // 从旧频率列表中移除
    std::list<NodePtr>& old_list = _freq_map[old_freq];
    old_list.remove(node);
    if (old_list.empty()) {
      _freq_map.erase(old_freq);
      if (old_freq == _min_freq) {
        // 本次只升频 1：旧最小桶清空后，新频率就是当前最小频率。
        _min_freq = new_freq;
      }
    }

    // 添加到新频率列表
    if (_freq_map.find(new_freq) == _freq_map.end()) {
      _freq_map[new_freq] = std::list<NodePtr>();
    }

    // 升频节点排在新桶尾部，同频率下优先保留最近进入该桶的节点。
    _freq_map[new_freq].push_back(node);
  }

  void evict_least_frequent() {
    // 先选择最小频率桶，再从桶头选出该频率内最早进入的节点。
    if (_freq_map.empty()) {
      return;
    }

    // 获取最小频率的列表
    auto& min_freq_list = _freq_map[_min_freq];
    if (min_freq_list.empty()) {
      return;
    }

    // 移除最少使用的节点
    NodePtr least_node = min_freq_list.front();
    min_freq_list.pop_front();

    // 如果该频率的列表为空，则删除该频率项
    if (min_freq_list.empty()) {
      _freq_map.erase(_min_freq);
      // 更新最小频率
      if (!_freq_map.empty()) {
        // 淘汰后频率可能存在间隔，利用有序 map 找实际最小值，不能直接加 1。
        _min_freq = _freq_map.begin()->first;
      }
    }

    // 将节点移到幽灵缓存
    if (_ghost_cache.size() >= _ghost_capacity) {
      remove_oldest_ghost();
    }
    add_to_ghost(least_node);

    // 从主缓存中移除
    _main_cache.erase(least_node->get_key());
  }

  bool remove_from_ghost(GhostNodePtr node) {
    if (!node) {
      return false;
    }

    auto prev = node->_prev.lock();
    auto next = node->_next;

    if (!prev || !next) {
      return false;
    }

    // 验证链表连接关系
    if (prev->_next != node || next->_prev.lock() != node) {
      return false;
    }

    // 连接前驱和后继
    prev->_next = next;
    next->_prev = prev;

    // 断开当前节点
    node->_next.reset();
    node->_prev.reset();

    return true;
  }

  void add_to_ghost(NodePtr evicted) {
    if (_ghost_capacity == 0) {
      return;
    }
    // 新建只携带 key 的历史节点，原 Value 随主节点释放。
    auto node = std::make_shared<GhostNodeType>(evicted->get_key(), nullptr);
    // LFU 的 ghost 从尾部追加，因此头部是最早的淘汰记录。
    node->_next = _ghost_tail;
    node->_prev = _ghost_tail->_prev;
    if (!_ghost_tail->_prev.expired()) {
      _ghost_tail->_prev.lock()->_next = node;
    }

    _ghost_tail->_prev = node;
    _ghost_cache[node->get_key()] = node;
  }

  void remove_oldest_ghost() {
    // 与尾部追加配对，从头部删除最旧历史，控制 ghost 的元数据规模。
    GhostNodePtr oldest_ghost = _ghost_head->_next;
    if (oldest_ghost != _ghost_tail) {
      remove_from_ghost(oldest_ghost);
      _ghost_cache.erase(oldest_ghost->get_key());
    }
  }

  size_t _capacity;
  size_t _ghost_capacity;
  size_t _transform_threshold;
  size_t _min_freq;

  std::mutex _mutex;

  NodeMap _main_cache;
  GhostMap _ghost_cache;
  FreqMap _freq_map;

  GhostNodePtr _ghost_head;
  GhostNodePtr _ghost_tail;
};

}  // namespace TLSS::CACHE

#endif  // __INC_COMMON_CACHE_ARC_LFU_H__
