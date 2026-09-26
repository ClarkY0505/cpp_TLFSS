#ifndef __COPY_CAPABILITY_POLICY_H__
#define __COPY_CAPABILITY_POLICY_H__

#include "copy_policy.h"
#include "cpu_capabilities.h"
#include "common/memory/tlss_memcpy.h"

namespace TLSS::MEMORY::INTERNAL {

// Apply only to plans selected for CopyBackend::Auto.
CopyPlan adapt_auto_plan_for_capabilities(CopyPlan plan,
                                          const CpuCapabilities& capabilities) noexcept;

CopyPlan select_safe_auto_copy_plan(std::size_t size, CopyHint hint,
                                    const CpuCapabilities& capabilities) noexcept;

bool is_backend_supported(CopyBackend backend, const CpuCapabilities& capabilities) noexcept;

CopyPlan make_copy_plan_for_backend(std::size_t size, CopyHint hint, CopyBackend backend,
                                    const CpuCapabilities& capabilities);

}  // namespace TLSS::MEMORY::INTERNAL

#endif  // __COPY_CAPABILITY_POLICY_H__
