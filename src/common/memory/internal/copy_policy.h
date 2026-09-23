#ifndef __COPY_POLICY_H__
#define __COPY_POLICY_H__

#include <cstddef>

#include "common/tlss_memcpy.h"

namespace TLSS::MEMORY::INTERNAL {

enum class CopyStrategy {
  LibcMemcpy,
  Avx2Cached,
  RepMovsb,
  DirectNt,
  ParallelNt,
};

struct CopyPlan {
  CopyStrategy strategy;
  std::size_t worker_count;
};

CopyPlan select_copy_plan(std::size_t size, CopyHint hint) noexcept;

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __COPY_POLICY_H__
