#ifndef __INC_COMMON_CACHE_ARC_LRU_H__
#define __INC_COMMON_CACHE_ARC_LRU_H__
#include <cassert>
#include "arc_node.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
namespace TLSS::CACHE {
// 主链表头部最近使用、尾部最久未使用；ghost 头部最新淘汰、尾部历史最旧。
template <typename Key, typename Value>
class ArcLru {
 public:
  using NodeType = ArcNode<Key, Value>;
  using NodePtr = std::shared_ptr<NodeType>;
  using NodeMap = std::unordered_map<Key, NodePtr>;
  // ghost 沿用节点链表结构，但不持有 Value，淘汰后可以释放实际数据。
  using GhostNodeType = ArcNode<Key, std::nullptr_t>;
  using GhostNodePtr = std::shared_ptr<GhostNodeType>;
  using GhostMap = std::unordered_map<Key, GhostNodePtr>;

  explicit ArcLru(size_t cap, size_t trans)
      : ArcLru(cap, trans, cap) {}

  ArcLru(size_t cap, size_t trans, size_t ghost_cap)
      : _capacity(cap)
      , _ghost_capacity(ghost_cap)
      , _transform_threshold(trans) {
    init_lists();
  }

  bool put(Key key, Value value) {
    // 容量会随 ghost 反馈变化，容量判断也必须放在锁内。
    std::lock_guard<std::mutex> lock(_mutex);
    if (_capacity == 0) {
      return false;
    }
    auto it = _main_cache.find(key);
    if (it != _main_cache.end()) {
      return update_existing_node(it->second,value);
    }
    // 重新驻留前清除同 key 历史，保证本分区的主缓存与 ghost 互斥。
    erase_ghost(key);
    return add_new_node(key, value);
  }

  bool get(Key key, Value& value, bool& should_Transform) {
    std::lock_guard<std::mutex> lock(_mutex);
    // 未命中也要清除输出标志，避免调用方沿用上一次读取的晋升结果。
    should_Transform = false;
    auto it = _main_cache.find(key);
    if (it != _main_cache.end()) {
      should_Transform = update_node_access(it->second);
      value = it->second->get_value();
      return true;
    }
    return false;
  }

  bool contain(const Key& key) {
    std::lock_guard<std::mutex> lock(_mutex);
    return _main_cache.find(key) != _main_cache.end();
  }

  // 晋升时移出 LRU，不产生淘汰历史。
  // 数据仍由 LFU 保存，若记为淘汰，会给后续容量调整提供错误反馈。
  bool erase(const Key& key) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _main_cache.find(key);
    if (it == _main_cache.end()) {
      return false;
    }
    remove_from_main(it->second);
    _main_cache.erase(it);
    return true;
  }

  // 此接口命中即消费历史，不是无副作用的存在性查询。
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
    // 配额缩小后立即淘汰超额节点；缩容造成的淘汰同样进入 ghost。
    --_capacity;
    while (_main_cache.size() > _capacity) {
      evict_least_recent();
    }
    return true;
  }

 private:
  // 调用方已持有 _mutex，供 put 和 check_ghost 复用，避免重复加锁。
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
    _main_head = std::make_shared<NodeType>();
    _main_tail = std::make_shared<NodeType>();
    _main_head->_next = _main_tail;
    _main_tail->_prev = _main_head;

    _ghost_head = std::make_shared<GhostNodeType>();
    _ghost_tail = std::make_shared<GhostNodeType>();
    _ghost_head->_next = _ghost_tail;
    _ghost_tail->_prev = _ghost_head;
  }

  bool update_existing_node(NodePtr node, const Value& value) {
    node->set_value(value);
    move_to_front(node);
    return true;
  }

  bool add_new_node(const Key& key, const Value& value) {
    if (_main_cache.size() >= _capacity) {
      // 驱逐最近最少访问
      evict_least_recent();
    }

    NodePtr new_node = std::make_shared<NodeType>(key, value);
    _main_cache[key] = new_node;
    add_to_front(new_node);
    return true;
  }

  bool update_node_access(NodePtr node) {
    // 新节点初始计数为 1，每次读取先更新最近使用顺序，再累加访问次数。
    // 达到阈值仅返回晋升建议，实际迁移由 ArcCache 在分片锁内完成。
    move_to_front(node);
    node->increment_access_count();
    return node->get_access_count() >= _transform_threshold;
  }

  void move_to_front(NodePtr node) {
    if (!remove_node(node)) {
      return;
    }
    add_to_front(node);
  }

  void add_to_front(NodePtr node) {
    /*
    // head <=> tail
    // node -> tail
    node->_next = _main_head->_next;
    // head <- node ->tail
    //   |<===========>|
    node->_prev = _main_head;
    // head <- node <=> tail
    //   |-------------->|
    _main_head->_next->_prev = node;
    // head <=> node ->tail
    _main_head->_next = node;
    */

    // 上下逻辑一致
    // 优化代码表示逻辑
    // 增加前置检查
    assert(node);
    assert(_main_head);
    assert(_main_head->_next);

    // node 必须是已经脱离链表的节点
    assert(node != _main_head);
    assert(node->_next == nullptr);
    assert(node->_prev.expired());

    auto first = _main_head->_next;

    node->_next = first;
    node->_prev = _main_head;

    first->_prev = node;
    _main_head->_next = node;
  }

  void evict_least_recent() {
    // 尾哨兵的前驱才是淘汰候选；空队列时不能把头哨兵当作数据节点。
    NodePtr least_recent = _main_tail->_prev.lock();
    if (!least_recent || least_recent == _main_head) {
      return;
    }

    // 1. 从主链表移除
    remove_from_main(least_recent);
    // 2. 添加幽灵缓存
    if (_ghost_cache.size() >= _ghost_capacity) {
      remove_oldest_ghost();
    }
    add_to_ghost(least_recent);

    // 3. 从主缓存映射中移除
    _main_cache.erase(least_recent->get_key());
  }

  // 主节点与 ghost 节点的 Value 类型不同，用模板复用相同的摘链操作。
  template <typename Node>
  bool remove_node(std::shared_ptr<Node> node) {
    /*
    if (!node->_prev.expired() && node->_next) {
      // prev <=> node <=> next
      std::shared_ptr<NodeType> prev = node->_prev.lock();
      // prev <- node <=> next
      // prev -> next
      prev->_next = node->_next;
      // prev <- node -> next;
      // prev <=> next
      node->_next->_prev = node->_prev;
      // shared_ptr 支持 nullptr 和 reset()
      // weak_ptr 只支持reset()
      // node->_next = nullptr;
      node->_next.reset();
      node->_prev.reset();
      // 优化代码表述
      // 上下两种方法是一样的
      auto prev = node->_prev.lock();
      auto next = node->_next;

      if (!prev || !next) {
        return;
      }

      // 连接前驱与后继
      prev->_next = next;
      next->_prev = prev;
      // 断开当前节点
      node->_next.reset();
      node->_prev.reset();
    }
    */
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

  void remove_from_main(NodePtr node) {
    if (!remove_node(node)) {
      return;
    }
  }

  void remove_from_ghost(GhostNodePtr node) {
    if (!remove_node(node)) {
      return;
    }
  }

  bool add_to_ghost(NodePtr evicted) {
    /*
    // 1. 重置节点的访问计数
    node->_access_count = 1;

    // 2.添加幽灵缓存的头部
    node->_next = _ghost_head->_next;
    node->_prev = _ghost_head;
    _ghost_head->_next->_prev = node;
    _ghost_head->_next = node;

    // 3.添加到幽灵缓存映射
    _ghost_cache[node->get_key()] = node;
    */
    if (!evicted || !_ghost_head || !_ghost_head->_next || _ghost_capacity == 0) {
      return false;
    }

    // 节点必须已经脱离原链表
    if (evicted->_next || !evicted->_prev.expired()) {
      return false;
    }

    // 单独复制 key，避免幽灵队列继续持有被淘汰节点及其 Value。
    auto node = std::make_shared<GhostNodeType>(evicted->get_key(), nullptr);

    // 先插入 HashMap，避免重复 Key
    auto [it, inserted] = _ghost_cache.try_emplace(node->get_key(), node);

    if (!inserted) {
      return false;
    }

    // 获取原先的第一个节点
    auto first = _ghost_head->_next;

    // 插入幽灵队列头部
    node->_next = first;
    node->_prev = _ghost_head;

    first->_prev = node;
    _ghost_head->_next = node;

    return true;
  }

  void remove_oldest_ghost() {
    // 新历史插入头部，因此超出历史预算时从尾部删除最早的记录。
    GhostNodePtr oldest_ghost = _ghost_tail->_prev.lock();
    if (!oldest_ghost || oldest_ghost == _ghost_head) {
      return;
    }

    remove_from_ghost(oldest_ghost);
    _ghost_cache.erase(oldest_ghost->get_key());
  }

  size_t _capacity;
  size_t _ghost_capacity;
  size_t _transform_threshold;  // 转换条件
  std::mutex _mutex;

  NodeMap _main_cache;
  GhostMap _ghost_cache;

  // 主链表
  NodePtr _main_head;
  NodePtr _main_tail;
  // 主链表
  GhostNodePtr _ghost_head;
  GhostNodePtr _ghost_tail;
};
}  // namespace TLSS::CACHE

#endif  // __INC_COMMON_CACHE_ARC_LRU_H__
