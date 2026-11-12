set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test")
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

add_executable(test_storage_engines test_storage_engines.cpp)
add_dependencies(test_storage_engines prepare_runtime_directories)
target_link_libraries(test_storage_engines PRIVATE transfer_lib)
add_test(NAME storage_engines_test COMMAND test_storage_engines)

# 真实文件系统操作；使用链接器包装注入 unlink/fsync 错误。
add_executable(test_object_removal test_object_removal.cpp)
add_dependencies(test_object_removal prepare_runtime_directories)
target_link_libraries(test_object_removal PRIVATE transfer_lib)
target_link_options(test_object_removal PRIVATE
    "-Wl,--wrap=unlink"
    "-Wl,--wrap=fsync"
)
# 每个场景独立进程，避免 object_root() 的静态缓存影响目录配置。
foreach(scenario IN ITEMS normal missing invalid_hash directory symlink
        unlink_retry unlink_error sync_retry sync_error)
    add_test(NAME object_removal_${scenario} COMMAND test_object_removal ${scenario})
    set_tests_properties(object_removal_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 复用删除测试的系统调用注入，验证启动时临时文件清理及恢复。
foreach(scenario IN ITEMS empty matching ignored types hard_link unlink_retry missing unlink_error
        partial_failure sync_retry sync_error crash)
    add_test(NAME object_temporary_cleanup_${scenario}
        COMMAND test_object_removal cleanup_${scenario})
    set_tests_properties(object_temporary_cleanup_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 只读枚举磁盘候选：验证排序、命名过滤、类型过滤及扫描失败传播。
foreach(scenario IN ITEMS empty sorted ignored types failure)
    add_test(NAME object_disk_scan_${scenario} COMMAND test_object_removal scan_${scenario})
    set_tests_properties(object_disk_scan_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 使用真实目录和文件锁验证独占访问，不启动存储服务或连接数据库。
add_executable(test_storage_root_lock test_storage_root_lock.cpp)
add_dependencies(test_storage_root_lock prepare_runtime_directories)
target_link_libraries(test_storage_root_lock PRIVATE transfer_lib)
foreach(scenario IN ITEMS exclusive independent exception invalid_root symlink invalid_lock
        process_exclusive process_exit)
    add_test(NAME storage_root_lock_${scenario} COMMAND test_storage_root_lock ${scenario})
    set_tests_properties(storage_root_lock_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_signup_transaction_boundary
    test_signup_transaction_boundary.cpp
    ${PROJECT_SOURCE_DIR}/src/auth/user_repository.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/metadata_root.cpp
)
add_dependencies(test_signup_transaction_boundary prepare_runtime_directories)
target_link_libraries(test_signup_transaction_boundary PRIVATE common_lib)
add_test(NAME signup_transaction_boundary_test COMMAND test_signup_transaction_boundary)

add_executable(test_tlss_memory_link test_tlss_memory_link.cpp)
add_dependencies(test_tlss_memory_link prepare_runtime_directories)
target_link_libraries(test_tlss_memory_link PRIVATE common_lib)
add_test(NAME tlss_memory_link_test COMMAND test_tlss_memory_link)

add_executable(test_tcp_server_shutdown test_tcp_server_shutdown.cpp)
add_dependencies(test_tcp_server_shutdown prepare_runtime_directories)
target_link_libraries(test_tcp_server_shutdown PRIVATE common_lib)
add_test(NAME tcp_server_shutdown_test COMMAND test_tcp_server_shutdown)

add_executable(test_thread_pool_async test_thread_pool_async.cpp)
add_dependencies(test_thread_pool_async prepare_runtime_directories)
target_link_libraries(test_thread_pool_async PRIVATE common_lib)
add_test(NAME thread_pool_async_test COMMAND test_thread_pool_async)
set_tests_properties(thread_pool_async_test PROPERTIES TIMEOUT 15)

# 使用进程内数据库替代实现测试实际的存储引擎。
# 通过查询和连接借用计数验证缓存命中，无需启动 MySQL 服务。
add_executable(test_metadata_cache
    test_metadata_cache.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/metadata_service.cpp
    # 元数据登记现在依赖对象仓储与显式上传事务。
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/upload_registration.cpp
)
add_dependencies(test_metadata_cache prepare_runtime_directories)
target_link_libraries(test_metadata_cache PRIVATE common_lib OpenSSL::Crypto)
foreach(scenario IN ITEMS list cd download failures writes failed_write unknown_commit concurrent stale_fill
        local_invalidation partition_concurrency ttl large statistics disabled arc shared config)
    add_test(NAME metadata_cache_${scenario} COMMAND test_metadata_cache ${scenario})
    set_tests_properties(metadata_cache_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# Compile the real FTP handlers with local database/authentication substitutes.
# Private access is limited to this regression target for callback/error injection.
add_executable(test_ftp_transfer
    test_ftp_transfer.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/transfer_task.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/upload_engine.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/download_engine.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/server.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/metadata_service.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/client.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_backfill.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
    # 文件登记与对象登记现在共同调用上传事务实现。
    ${PROJECT_SOURCE_DIR}/src/ftp/upload_registration.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_garbage_collector.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_gc_transaction.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_gc_worker.cpp
)
add_dependencies(test_ftp_transfer prepare_runtime_directories)
target_compile_options(test_ftp_transfer PRIVATE -fno-access-control)
target_link_libraries(test_ftp_transfer PRIVATE transfer_lib common_lib OpenSSL::Crypto)
target_link_options(test_ftp_transfer PRIVATE "-Wl,--wrap=fsync")
foreach(scenario IN ITEMS
        commit_hash upload download error_frame busy_reply receive_error
        control_limit source_type source_fifo upload_chunked client_download shutdown_wait
        cold_send source_cancel stale_read source_eof client_upload empty_send reader_error legacy_send
        metadata_failure metadata_unknown stale_prepare upload_cancel task_isolation
        metrics_auth metrics_snapshots metrics_stale_commit metrics_pool metrics_storage
        # 启动准备：对象表缺失，以及表存在但历史清单为空。
        prepare_missing_table prepare_empty_list
        prepare_recovery_query_failure prepare_recovery_invalid_hash prepare_recovery_skip
        prepare_recovery_resume prepare_recovery_referenced prepare_recovery_unknown
        prepare_cleanup_sync_failure prepare_cleanup_started
        prepare_orphan_valid prepare_orphan_corrupt prepare_orphan_existing
        prepare_orphan_lookup_failure prepare_orphan_insert_failure
        gc_start_failure gc_late_start_failure gc_shutdown_idle gc_shutdown_wait
        gc_unstarted_cleanup)
    add_test(NAME ftp_transfer_${scenario} COMMAND test_ftp_transfer ${scenario})
    set_tests_properties(ftp_transfer_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_write_scheduler test_write_scheduler.cpp)
add_dependencies(test_write_scheduler prepare_runtime_directories)
target_link_libraries(test_write_scheduler PRIVATE common_lib)
add_test(NAME write_scheduler_test COMMAND test_write_scheduler)
set_tests_properties(write_scheduler_test PROPERTIES TIMEOUT 15)

add_executable(test_tcp_write_scheduler test_tcp_write_scheduler.cpp)
add_dependencies(test_tcp_write_scheduler prepare_runtime_directories)
target_link_libraries(test_tcp_write_scheduler PRIVATE common_lib)
add_test(NAME tcp_write_scheduler_test COMMAND test_tcp_write_scheduler)
set_tests_properties(tcp_write_scheduler_test PROPERTIES TIMEOUT 15)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME ftp_cd_lookup_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ftp_cd.py)
endif()

if(GTest_FOUND)
    add_executable(test_ftp_server test_ftp_server.cpp)
    add_dependencies(test_ftp_server prepare_runtime_directories)
    target_link_libraries(test_ftp_server
        ftp_lib
        common_lib
        GTest::GTest
        GTest::Main
    )
    add_test(NAME ftp_server_test COMMAND test_ftp_server)

    add_executable(test_monitor_timer test_monitor_timer.cpp)
    add_dependencies(test_monitor_timer prepare_runtime_directories)
    target_link_libraries(test_monitor_timer
        monitor_lib
        GTest::GTest
        GTest::Main
    )
    add_test(NAME monitor_timer_test COMMAND test_monitor_timer)
endif()

# Production observability components; no real database is required.
add_executable(test_storage_metrics test_storage_metrics.cpp)
target_link_libraries(test_storage_metrics PRIVATE Threads::Threads)
add_dependencies(test_storage_metrics prepare_runtime_directories)
add_test(NAME storage_metrics_test COMMAND test_storage_metrics)

add_executable(test_system_resource_sampler test_system_resource_sampler.cpp)
target_link_libraries(test_system_resource_sampler PRIVATE observability_lib)
add_dependencies(test_system_resource_sampler prepare_runtime_directories)
add_test(NAME system_resource_sampler_test COMMAND test_system_resource_sampler)
set_tests_properties(system_resource_sampler_test PROPERTIES TIMEOUT 15)

add_executable(test_storage_monitor_runtime test_storage_monitor_runtime.cpp)
target_link_libraries(test_storage_monitor_runtime PRIVATE observability_lib)
add_dependencies(test_storage_monitor_runtime prepare_runtime_directories)

add_executable(test_object_backfill
    test_object_backfill.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_backfill.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
)
add_dependencies(test_object_backfill prepare_runtime_directories)
target_link_libraries(test_object_backfill PRIVATE transfer_lib)
add_test(NAME object_backfill_test COMMAND test_object_backfill)
set_tests_properties(object_backfill_test PROPERTIES TIMEOUT 15)

# 使用真实仓储；测试文件替代连接和事务，不访问真实 MySQL。
add_executable(test_object_reference
    test_object_reference.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
)
add_dependencies(test_object_reference prepare_runtime_directories)
target_link_libraries(test_object_reference PRIVATE common_lib)
add_test(NAME object_reference_test COMMAND test_object_reference)
set_tests_properties(object_reference_test PROPERTIES TIMEOUT 15)

# 使用真实对象仓储和模拟数据库边界，不连接 MySQL。
add_executable(test_object_gc_repository
    test_object_gc_repository.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
)
add_dependencies(test_object_gc_repository prepare_runtime_directories)
target_link_libraries(test_object_gc_repository PRIVATE common_lib)
add_test(NAME object_gc_repository_test COMMAND test_object_gc_repository)
set_tests_properties(object_gc_repository_test PROPERTIES TIMEOUT 15)

# 使用真实 GC 准备、收尾事务及仓储，替换数据库连接和事务边界。
add_executable(test_object_gc_transaction
    test_object_gc_transaction.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_gc_transaction.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
)
add_dependencies(test_object_gc_transaction prepare_runtime_directories)
target_link_libraries(test_object_gc_transaction PRIVATE common_lib)
foreach(scenario IN ITEMS invalid_arguments begin_and_lookup malformed_results references
        unreferenced write_failures commit_results exceptions
        finalize_invalid_arguments finalize_lookup finalize_malformed_results finalize_states
        finalize_references finalize_delete_failures finalize_commit_results finalize_exceptions)
    add_test(NAME object_gc_transaction_${scenario} COMMAND test_object_gc_transaction ${scenario})
    set_tests_properties(object_gc_transaction_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 单对象 GC 编排使用真实调用流程和协调器，边界替代实现不访问 MySQL 或磁盘。
# 不链接 ftp_lib/transfer_lib，避免替代事务、存储函数与生产定义重复。
add_executable(test_object_garbage_collector
    test_object_garbage_collector.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_garbage_collector.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_coordinator.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
)
add_dependencies(test_object_garbage_collector prepare_runtime_directories)
target_link_libraries(test_object_garbage_collector PRIVATE common_lib Threads::Threads)
foreach(scenario IN ITEMS invalid_arguments connection_failure prepare_results finalize_results
        exceptions hash_lock recovery_invalid_arguments recovery_listing recovery_malformed
        recovery_multiple recovery_collection_failure recovery_unknown recovery_exceptions
        batch_invalid_arguments batch_listing batch_malformed batch_progress batch_failure
        batch_unknown batch_exceptions batch_limit)
    add_test(NAME object_garbage_collector_${scenario}
        COMMAND test_object_garbage_collector ${scenario})
    set_tests_properties(object_garbage_collector_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 使用真实 GC 调度线程，批处理与连接池由测试替代，不访问数据库或对象目录。
add_executable(test_object_gc_worker
    test_object_gc_worker.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_gc_worker.cpp
)
add_dependencies(test_object_gc_worker prepare_runtime_directories)
# 仅测试目标可只读探测停止标志，并注入条件变量通知。
target_compile_options(test_object_gc_worker PRIVATE -fno-access-control)
target_link_libraries(test_object_gc_worker PRIVATE common_lib Threads::Threads ${CMAKE_DL_LIBS})
foreach(scenario IN ITEMS configuration lifecycle stop_before_start interval progress failures
        creation_failure destructor concurrent_stop logging_failure)
    add_test(NAME object_gc_worker_${scenario} COMMAND test_object_gc_worker ${scenario})
    set_tests_properties(object_gc_worker_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 使用真实登记事务和对象仓储，数据库连接及事务边界由测试替代。
add_executable(test_upload_registration
    test_upload_registration.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/upload_registration.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
)
add_dependencies(test_upload_registration prepare_runtime_directories)
target_link_libraries(test_upload_registration PRIVATE common_lib)
add_test(NAME upload_registration_test COMMAND test_upload_registration)
set_tests_properties(upload_registration_test PROPERTIES TIMEOUT 15)

foreach(scenario IN ITEMS thresholds schema lifecycle resources transport configuration self_stop)
    add_test(NAME storage_monitor_${scenario} COMMAND test_storage_monitor_runtime ${scenario})
    set_tests_properties(storage_monitor_${scenario} PROPERTIES TIMEOUT 15)
endforeach()
if(Python3_Interpreter_FOUND)
    add_test(NAME storage_collector_smoke
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_storage_collector.py
                $<TARGET_FILE:storage-monitor-collector>)
    set_tests_properties(storage_collector_smoke PROPERTIES TIMEOUT 30)
endif()
