#include "copy_policy.h"

namespace TLSS::MEMORY::INTERNAL {
CopyPlan select_copy_plan(std::size_t size, CopyHint hint) noexcept {
  constexpr std::size_t k_avx2_threshold = 2ULL * 1024;
  constexpr std::size_t k_glibc_threshold = 3ULL * 1024;
  constexpr std::size_t k_direct_nt_threshold = 8ULL * 1024;
  constexpr std::size_t k_parallel_4_threshold = 64ULL * 1024;
  constexpr std::size_t k_parallel_8_threshold = 128ULL * 1024;

  if (hint == CopyHint::Default) {
    if (size <= k_avx2_threshold) {
      return {CopyStrategy::Avx2Cached, 1};
    }

    return {CopyStrategy::RepMovsb, 1};
  }

   if (size <= k_avx2_threshold) {
    return {
        CopyStrategy::Avx2Cached,
        1
    };
  }

  if (size < k_glibc_threshold) {
    return {CopyStrategy::LibcMemcpy, 1};
  }

  if (size < k_direct_nt_threshold) {
    return {CopyStrategy::Avx2Cached, 1};
  }

  if (size < k_parallel_4_threshold) {
    return {CopyStrategy::DirectNt, 1};
  }

  if (size < k_parallel_8_threshold) {
    return {CopyStrategy::ParallelNt, 4};
  }

  return {CopyStrategy::ParallelNt, 8};
}

}  // namespace TLSS::MEMORY::INTERNAL
