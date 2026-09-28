#ifndef __INC_COMMON_UTILE_UTILE_H__
#define __INC_COMMON_UTILE_UTILE_H__
#include <cstddef>
#include <cstring>

namespace TLSS::UTIL {
inline void memzero(void* ptr, std::size_t size) {
  std::memset(ptr, 0, size);
}

template <typename To, typename From>
inline To implicit_cast(const From& value) {
  return value;
}
}  // namespace TLSS::UTIL
#endif  // __INC_COMMON_UTILE_UTILE_H__
