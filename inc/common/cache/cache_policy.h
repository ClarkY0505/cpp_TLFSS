#ifndef __INC_COMMON_CACHE_CACHE_POLICY_H__
#define __INC_COMMON_CACHE_CACHE_POLICY_H__

namespace TLSS::CACHE {
template <typename Key, typename Value>
class CachePolicy {
 public:
  virtual ~CachePolicy(){};

  // 添加缓存接口
  virtual void put(Key key, Value value) = 0;

  // key是传入参数  访问到的值以传出参数的形式返回 | 访问成功返回true
  virtual bool get(Key key, Value& value) = 0;
  // 如果缓存中能找到key，则直接返回value
  virtual Value get(Key key) = 0;
};
}  // namespace TLSS::CACHE

#endif  // __INC_COMMON_CACHE_CACHE_POLICY_H__
