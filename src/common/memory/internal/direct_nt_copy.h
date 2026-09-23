#ifndef __DIRECT_NT_COPY_H__
#define __DIRECT_NT_COPY_H__

#include <cstddef>

namespace TLSS::MEMORY::INTERNAL {

void* direct_nt_copy(void* dst, const void* src, std::size_t size);

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __DIRECT_NT_COPY_H__
