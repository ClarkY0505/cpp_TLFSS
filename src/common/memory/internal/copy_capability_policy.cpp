#include "copy_capability_policy.h"

#include <stdexcept>

namespace TLSS::MEMORY::INTERNAL {

CopyPlan adapt_auto_plan_for_capabilities(CopyPlan plan,
                                          const CpuCapabilities& capabilities) noexcept {
  switch (plan.strategy) {
    case CopyStrategy::LibcMemcpy:
    case CopyStrategy::RepMovsb:
      return plan;

    case CopyStrategy::Avx2Cached:
    case CopyStrategy::DirectNt:
    case CopyStrategy::ParallelNt:
      if (capabilities.avx2_usable) {
        return plan;
      }
      return {CopyStrategy::LibcMemcpy, 1};
  }

  return {CopyStrategy::LibcMemcpy, 1};
}

CopyPlan select_safe_auto_copy_plan(std::size_t size, CopyHint hint,
                                    const CpuCapabilities& capabilities) noexcept {
  return adapt_auto_plan_for_capabilities(select_copy_plan(size, hint), capabilities);
}

bool is_backend_supported(CopyBackend backend, const CpuCapabilities& capabilities) noexcept {
  switch (backend) {
    case CopyBackend::Auto:
    case CopyBackend::RepMovsb:
      return true;

    case CopyBackend::Avx2:
    case CopyBackend::NonTemporal:
      // The current non-temporal kernel uses AVX2 YMM registers.
      return capabilities.avx2_usable;
  }

  return false;
}

CopyPlan make_copy_plan_for_backend(std::size_t size, CopyHint hint, CopyBackend backend,
                                    const CpuCapabilities& capabilities) {
  switch (backend) {
    case CopyBackend::Auto:
      return select_safe_auto_copy_plan(size, hint, capabilities);

    case CopyBackend::Avx2:
      if (!is_backend_supported(backend, capabilities)) {
        throw std::runtime_error("TLSS memcpy: AVX2 backend is not supported");
      }
      return {CopyStrategy::Avx2Cached, 1};

    case CopyBackend::RepMovsb:
      return {CopyStrategy::RepMovsb, 1};

    case CopyBackend::NonTemporal:
      if (!is_backend_supported(backend, capabilities)) {
        throw std::runtime_error("TLSS memcpy: non-temporal backend is not supported");
      }
      return {CopyStrategy::DirectNt, 1};
  }

  throw std::runtime_error("TLSS memcpy: invalid backend");
}

}  // namespace TLSS::MEMORY::INTERNAL
