#include "copy_executor.h"
#include "direct_nt_copy.h"
#include "memory_runtime.h"

#include <cstring>
#include <stdexcept>

extern "C" {

void* tlss_avx2_memcpy_5(void* dst, const void* src, std::size_t size);

void* rep_memcpy(void* dst, const void* src, std::size_t size);

void* tlss_avx2_nt_memcpy_2stream(void* dst, const void* src, std::size_t size);
}

namespace TLSS::MEMORY::INTERNAL {

void* execute_copy_plan(const CopyPlan& plan, MemoryRuntime* runtime, void* dst, const void* src,
                        std::size_t size) {
  switch (plan.strategy) {
    case CopyStrategy::Avx2Cached:
      return tlss_avx2_memcpy_5(dst, src, size);

    case CopyStrategy::LibcMemcpy:
      return std::memcpy(dst, src, size);

    case CopyStrategy::RepMovsb:
      return rep_memcpy(dst, src, size);

    case CopyStrategy::DirectNt:
      /* return tlss_avx2_nt_memcpy_2stream(dst, src, size); */
      return direct_nt_copy(dst, src, size);

    case CopyStrategy::ParallelNt: {
      if (runtime == nullptr) {
        return direct_nt_copy(dst, src, size);
      }

      return runtime->execute_parallel_copy(plan.worker_count, dst, src, size);
    }
  }

  throw std::logic_error("invalid copy strategy");
}

}  // namespace TLSS::MEMORY::INTERNAL
