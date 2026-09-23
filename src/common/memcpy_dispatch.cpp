#include "common/tlss_memcpy.h"
#include "memory/internal/copy_capability_policy.h"
#include "memory/internal/copy_executor.h"
#include "memory/internal/cpu_capabilities.h"
#include "memory/internal/memory_runtime.h"

#include <cstddef>

namespace TLSS::MEMORY {

namespace {
TLSS::MEMORY::INTERNAL::MemoryRuntime& get_memory_runtime() {
  // The shared runtime must remain available to other static destructors.
  // The function-local static pointer retains thread-safe first initialization
  // and retries initialization if construction throws.
  static TLSS::MEMORY::INTERNAL::MemoryRuntime* const runtime =
      new TLSS::MEMORY::INTERNAL::MemoryRuntime();

  return *runtime;
}

TLSS::MEMORY::INTERNAL::MemoryRuntime* try_get_memory_runtime() noexcept {
  try {
    return &get_memory_runtime();
  } catch (...) {
    return nullptr;
  }
}

}  // namespace

void* memcpy(void* dst, const void* src, std::size_t size) {
  return TLSS::MEMORY::memcpy(dst, src, size, CopyBackend::Auto, CopyHint::Default);
}

void* memcpy(void* dst, const void* src, std::size_t size, CopyBackend backend, CopyHint hint) {
  if (size == 0) {
    return dst;
  }

  const auto& capabilities = INTERNAL::get_cpu_capabilities();
  const INTERNAL::CopyPlan plan =
      INTERNAL::make_copy_plan_for_backend(size, hint, backend, capabilities);

  INTERNAL::MemoryRuntime* runtime = nullptr;
  if (plan.strategy == INTERNAL::CopyStrategy::ParallelNt) {
    runtime = try_get_memory_runtime();
  }

  return INTERNAL::execute_copy_plan(plan, runtime, dst, src, size);
}

}  // namespace TLSS::MEMORY
