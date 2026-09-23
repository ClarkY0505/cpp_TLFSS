#ifndef __INC_COMMON_TLSS_MEMCPY_H__
#define __INC_COMMON_TLSS_MEMCPY_H__

#include <cstddef>

namespace TLSS::MEMORY {
enum class CopyBackend {
  Auto,
  Avx2,
  RepMovsb,
  NonTemporal,
};

enum class CopyHint {
  Default,
  Streaming,
};

void* memcpy(void* dst, const void* src, std::size_t size);
void* memcpy(void* dst, const void* src, std::size_t size, CopyBackend backend, CopyHint hint);

}  // namespace TLSS::MEMORY

#endif  // __INC_COMMON_TLSS_MEMCPY_H__
