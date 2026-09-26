#ifndef __PARALLEL_NT_COPY_H__
#define __PARALLEL_NT_COPY_H__

#include <cstddef>
#include <vector>

namespace TLSS::MEMORY::INTERNAL {

class ParallelCopyPool;

void* parallel_nt_copy(ParallelCopyPool& pool, const std::vector<std::size_t>& active_worker_indices, void* dst, const void* src, std::size_t size);

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __PARALLEL_NT_COPY_H__
