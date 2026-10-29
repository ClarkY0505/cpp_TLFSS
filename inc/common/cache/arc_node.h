#ifndef __INC_COMMON_CACHE_ARC_NODE_H__
#define __INC_COMMON_CACHE_ARC_NODE_H__

#include <cstddef>
#include <memory>
namespace TLSS::CACHE {
template <typename Key, typename Value>
class ArcNode {
 public:
  template <typename K, typename V>
  friend class ArcLru;
  template <typename K, typename V>
  friend class ArcLfu;

  ArcNode()
      : _access_count(1)
      , _next(nullptr) {}
  ArcNode(Key key, Value value)
      : _key(key)
      , _value(value)
      , _access_count(1)
      , _next(nullptr) {}

  // Getter
  Key get_key() const {
    return _key;
  }
  Value get_value() const {
    return _value;
  }
  size_t get_access_count() const {
    return _access_count;
  }

  // Setter
  void set_value(const Value& value) {
    _value = value;
  }
  void increment_access_count() {
    ++_access_count;
  }

 private:
  Key _key;
  Value _value;
  size_t _access_count;
  std::weak_ptr<ArcNode> _prev;
  std::shared_ptr<ArcNode> _next;
};
}  // namespace TLSS::CACHE

#endif  // __INC_COMMON_CACHE_ARC_NODE_H__
