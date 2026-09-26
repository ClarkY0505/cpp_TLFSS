#ifndef __COPY_EXECUTOR_H__
#define __COPY_EXECUTOR_H__

#include <cstddef>

#include "copy_policy.h"
#include "memory_runtime.h"

namespace TLSS::MEMORY::INTERNAL {

class ParallelCopyPool;

void* execute_copy_plan(const CopyPlan& plan, MemoryRuntime* runtime, void* dst, const void* src,
                        std::size_t size);

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __COPY_EXECUTOR_H__
