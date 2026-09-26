add_library(tlss_memory SHARED
    memcpy_dispatch.cpp
    internal/copy_capability_policy.cpp
    internal/copy_executor.cpp
    internal/copy_partition.cpp
    internal/copy_policy.cpp
    internal/cpu_affinity.cpp
    internal/cpu_capabilities.cpp
    internal/cpu_core_type.cpp
    internal/cpu_topology.cpp
    internal/direct_nt_copy.cpp
    internal/memory_runtime.cpp
    internal/parallel_copy_pool.cpp
    internal/parallel_nt_copy.cpp
    internal/worker_selection.cpp
    avx2_memcpy_v6.asm
    rep_memcpy.asm
    tlss_avx2_nt_memcpy_2stream.asm
)

set_target_properties(tlss_memory PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/lib"
)

target_include_directories(tlss_memory
    PUBLIC ${PROJECT_SOURCE_DIR}/inc
    PRIVATE ${PROJECT_SOURCE_DIR}/inc/common/memory/internal
)

target_link_libraries(tlss_memory PRIVATE Threads::Threads)

install(TARGETS tlss_memory LIBRARY DESTINATION lib)
install(FILES ${PROJECT_SOURCE_DIR}/inc/common/memory/tlss_memcpy.h DESTINATION include/common/memory)
