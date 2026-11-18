set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test")
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# 公共目录路径校验：纯字符串测试，无需数据库、网络或服务启动。
add_executable(test_directory_path_validation test_directory_path_validation.cpp)
add_dependencies(test_directory_path_validation prepare_runtime_directories)
target_link_libraries(test_directory_path_validation PRIVATE common_lib)
foreach(scenario IN ITEMS valid relative separators dot_components component_limits path_limits control_bytes)
    add_test(NAME directory_path_validation_${scenario}
        COMMAND test_directory_path_validation ${scenario})
    set_tests_properties(directory_path_validation_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 路径解析仅处理虚拟路径字符串；与校验测试复用可执行文件，不查询实际目录。
foreach(scenario IN ITEMS absolute relative dot parent root_boundary invalid_current invalid_argument limits literal_names)
    add_test(NAME directory_path_resolution_${scenario}
        COMMAND test_directory_path_validation resolve_${scenario})
    set_tests_properties(directory_path_resolution_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

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
    ${PROJECT_SOURCE_DIR}/src/ftp/file_deletion.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_creation.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_deletion.cpp
)
add_dependencies(test_metadata_cache prepare_runtime_directories)
target_link_libraries(test_metadata_cache PRIVATE common_lib OpenSSL::Crypto)
foreach(scenario IN ITEMS list cd download failures writes failed_write unknown_commit concurrent stale_fill
        local_invalidation partition_concurrency ttl large statistics disabled arc shared config)
    add_test(NAME metadata_cache_${scenario} COMMAND test_metadata_cache ${scenario})
    set_tests_properties(metadata_cache_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 双目录协调仅测试真实缓存，无数据库替代或服务启动。
add_executable(test_metadata_cache_pair test_metadata_cache_pair.cpp)
add_dependencies(test_metadata_cache_pair prepare_runtime_directories)
target_link_libraries(test_metadata_cache_pair PRIVATE common_lib Threads::Threads)
foreach(scenario IN ITEMS success failure exception nonstandard_exception same_directory collision
        readers reverse_order independent stale_fill removed_directory disabled move_only)
    add_test(NAME metadata_cache_pair_${scenario} COMMAND test_metadata_cache_pair ${scenario})
    set_tests_properties(metadata_cache_pair_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 三类缓存共用的子树版本校验及范围协调，不连接数据库、不创建网络套接字。
add_executable(test_metadata_cache_subtree
    test_metadata_cache_subtree.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/subtree_coordinator.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/subtree_versions.cpp
)
add_dependencies(test_metadata_cache_subtree prepare_runtime_directories)
# 仅用于注入版本耗尽边界、发布子树版本和观察实际等待队列。
target_compile_options(test_metadata_cache_subtree PRIVATE -fno-access-control)
target_link_libraries(test_metadata_cache_subtree PRIVATE common_lib Threads::Threads)
foreach(scenario IN ITEMS nonzero self ancestor root repeated failed_load directory_version
        disabled_admission overflow_boundary collisions scope_hit scope_miss scope_disabled
        scope_independent exception_release empty_list write_success write_failure write_exception
        write_nonstandard_exception write_collision write_move_only write_scope_parent
        write_scope_descendant)
    add_test(NAME metadata_cache_subtree_${scenario}
        COMMAND test_metadata_cache_subtree ${scenario})
    set_tests_properties(metadata_cache_subtree_${scenario} PROPERTIES TIMEOUT 15)
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
    ${PROJECT_SOURCE_DIR}/src/ftp/file_deletion.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_creation.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_deletion.cpp
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
        gc_unstarted_cleanup
        rm_success rm_help rm_invalid rm_unauthenticated rm_transfer_busy rm_directory_busy
        rm_queue_full rm_pool_stopped rm_missing rm_delete_failure rm_zero rm_exception
        rm_nonstandard_exception rm_unknown_applied rm_unknown_not_applied rm_pending
        rm_stale_session rm_expired_session rm_stale_identity rm_stale_directory
        rm_stale_login rm_stale_pending rm_disconnect rm_stopping
        mkdir_success mkdir_unknown_applied mkdir_unknown_not_applied mkdir_sql_failure
        mkdir_zero mkdir_exception mkdir_nonstandard_exception mkdir_missing_parent mkdir_invalid
        mkdir_queue_full mkdir_pool_stopped mkdir_pending mkdir_disconnect mkdir_unauthenticated
        # 空目录删除：命令分发、四态响应、参数、并发会话与停机边界。
        rmdir_success rmdir_help rmdir_invalid rmdir_unauthenticated
        rmdir_transfer_busy rmdir_directory_busy rmdir_queue_full rmdir_pool_stopped
        rmdir_missing rmdir_missing_parent rmdir_not_empty_file rmdir_not_empty_directory
        rmdir_child_query_failure rmdir_child_malformed rmdir_borrow_failure rmdir_borrow_exception
        rmdir_delete_failure rmdir_zero rmdir_exception rmdir_nonstandard_exception
        rmdir_unknown_applied rmdir_unknown_not_applied rmdir_pending
        rmdir_stale_session rmdir_expired_session rmdir_stale_identity rmdir_stale_directory
        rmdir_stale_login rmdir_stale_pending rmdir_disconnect rmdir_stopping rmdir_stopping_entry
        rmdir_plain rmdir_literal_flag
        # -r 路由与三态响应：真实线程和 IO 回调、递归事务及部分删除后的故障。
        rmtree_success rmtree_empty rmtree_plain rmtree_literal_flag rmtree_whitespace rmtree_invalid
        rmtree_unauthenticated rmtree_transfer_busy rmtree_directory_busy rmtree_queue_full rmtree_pool_stopped
        rmtree_missing rmtree_missing_parent rmtree_child_query_failure rmtree_child_malformed
        rmtree_borrow_failure rmtree_borrow_exception rmtree_delete_failure rmtree_zero
        rmtree_exception rmtree_nonstandard_exception rmtree_unknown_applied rmtree_unknown_not_applied
        rmtree_pending rmtree_stale_session rmtree_expired_session rmtree_stale_identity
        rmtree_stale_directory rmtree_stale_login rmtree_stale_pending rmtree_disconnect
        rmtree_stopping rmtree_stopping_entry
        # 重命名业务任务：当前直接提交线程池，验证三态回复及 IO 会话检查。
        rename_task_success rename_task_sql_failure rename_task_zero rename_task_exception
        rename_task_nonstandard_exception rename_task_missing rename_task_unknown_applied
        rename_task_unknown_not_applied rename_task_pending rename_task_stale_session rename_task_expired_session
        rename_task_stale_identity rename_task_stale_directory rename_task_stale_login rename_task_stale_pending
        rename_task_disconnect rename_task_stopping
        rename_parse_invalid rename_parse_unauthenticated rename_parse_transfer_busy
        rename_parse_directory_busy rename_parse_stopping
        # rename 从实际命令分发进入线程池，覆盖任务绑定和拒绝后的状态恢复。
        rename_command_success rename_command_help rename_command_invalid rename_command_plain
        rename_command_flag_names rename_command_literal_names rename_command_whitespace
        rename_command_unauthenticated rename_command_directory_busy rename_command_queue_full rename_command_pool_stopped
        rename_command_sql_failure rename_command_zero rename_command_exception rename_command_nonstandard_exception
        rename_command_missing rename_command_unknown_applied rename_command_unknown_not_applied rename_command_pending
        rename_command_stale_session rename_command_expired_session rename_command_stale_identity rename_command_stale_directory
        rename_command_stale_login rename_command_stale_pending rename_command_disconnect rename_command_stopping
        # 移动业务任务：真实线程池、三态结果与过期会话的 IO 回调隔离。
        move_task_success move_task_sql_failure move_task_zero move_task_exception
        move_task_nonstandard_exception move_task_missing move_task_unknown_applied
        move_task_unknown_not_applied move_task_pending move_task_stale_session move_task_expired_session
        move_task_stale_identity move_task_stale_directory move_task_stale_login move_task_stale_pending
        move_task_disconnect move_task_stopping
        # 移动入口：参数解析、路径格式和会话状态；拒绝输入不得进入数据库。
        move_parse_syntax move_parse_file_name move_parse_target move_parse_unauthenticated
        move_parse_transfer_busy move_parse_directory_busy move_parse_stopping
        # 移动入口提交：验证参数绑定、pending、三态响应和线程池拒绝时的恢复。
        move_submit_success move_submit_sql_failure move_submit_zero move_submit_exception
        move_submit_nonstandard_exception move_submit_missing move_submit_unknown_applied
        move_submit_unknown_not_applied move_submit_pending move_submit_stale_session move_submit_expired_session
        move_submit_stale_identity move_submit_stale_directory move_submit_stale_login move_submit_stale_pending
        move_submit_disconnect move_submit_stopping move_submit_queue_full move_submit_pool_stopped
        # move 从真实网络命令进入，验证分发、帮助信息以及既有异步结果和会话边界。
        move_command_success move_command_help move_command_invalid move_command_whitespace
        move_command_sql_failure move_command_zero move_command_exception move_command_nonstandard_exception
        move_command_missing move_command_unknown_applied move_command_unknown_not_applied move_command_pending
        move_command_stale_session move_command_expired_session move_command_stale_identity
        move_command_stale_directory move_command_stale_login move_command_stale_pending
        move_command_disconnect move_command_stopping move_command_queue_full move_command_pool_stopped
        # 移动目标解析：根/非根源目录、相对/绝对路径、.、.. 与虚拟根边界。
        move_command_relative_root move_command_relative_nested move_command_absolute_nested
        move_command_parent move_command_absolute_parent move_command_dot move_command_target_root
        move_command_root_boundary move_command_relative_multi
        # cd 入口：单字段解析、引号、状态检查及队列拒绝恢复；执行真实网络和线程池。
        cd_parse_invalid cd_parse_unauthenticated cd_parse_transfer_busy cd_parse_directory_busy cd_parse_stopping
        cd_command_plain cd_command_quoted cd_command_absolute cd_command_relative cd_command_parent
        cd_command_dot cd_command_root cd_command_missing cd_command_whitespace cd_command_invalid
        cd_command_queue_full cd_command_pool_stopped cd_command_pending
        # cd 任务收尾：异常仍回复并恢复 pending，过期会话和断线不得消费旧查询结果。
        cd_command_borrow_failure cd_command_borrow_exception cd_command_query_failure
        cd_command_query_exception cd_command_nonstandard_exception
        cd_command_stale_session cd_command_expired_session cd_command_missing_session
        cd_command_stale_identity cd_command_stale_directory cd_command_stale_login cd_command_stale_pending
        cd_command_disconnect cd_command_stopping)
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

# 真实删除事务函数；数据库连接与事务边界由测试替代，不连接 MySQL。
add_executable(test_file_deletion
    test_file_deletion.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/file_deletion.cpp
)
add_dependencies(test_file_deletion prepare_runtime_directories)
target_link_libraries(test_file_deletion PRIVATE common_lib)
foreach(scenario IN ITEMS invalid_arguments path_limits begin_failure existing_transaction
        user_results parent_results file_results delete_results committed unknown_applied
        unknown_not_applied commit_not_active exceptions repeat object_retention)
    add_test(NAME file_deletion_${scenario} COMMAND test_file_deletion ${scenario})
    set_tests_properties(file_deletion_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 文件同目录重命名：真实事务函数、工作副本回滚及三态提交；不访问 MySQL。
add_executable(test_file_rename
    test_file_rename.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/file_rename.cpp
)
add_dependencies(test_file_rename prepare_runtime_directories)
target_link_libraries(test_file_rename PRIVATE common_lib)
foreach(scenario IN ITEMS invalid_arguments path_limits begin_failure existing_transaction
        user_results parent_results source_results target_results missing_source source_directory
        parent_not_directory exact_path duplicate_file duplicate_directory case_collision namespace_scope
        update_results committed root_parent literal_name unchanged unchanged_missing case_only
        unknown_applied unknown_not_applied commit_not_active commit_fallback exceptions repeat)
    add_test(NAME file_rename_${scenario} COMMAND test_file_rename ${scenario})
    set_tests_properties(file_rename_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 跨目录文件移动：运行实际事务函数，验证源目标归属、路径、引用保留和原子提交。
add_executable(test_file_move
    test_file_move.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/file_move.cpp
)
add_dependencies(test_file_move prepare_runtime_directories)
target_link_libraries(test_file_move PRIVATE common_lib)
foreach(scenario IN ITEMS invalid_arguments path_limits begin_failure existing_transaction user_results
        source_directory_results target_directory_results source_results conflict_results
        missing_source_directory missing_target_directory source_not_directory target_not_directory
        foreign_target missing_source source_is_directory source_exact_path target_exact_path file_exact_path
        duplicate_file duplicate_directory case_collision update_results committed root_source root_target
        literal_name same_directory same_directory_missing same_directory_unknown unknown_applied unknown_not_applied
        commit_not_active commit_fallback exceptions repeat)
    add_test(NAME file_move_${scenario} COMMAND test_file_move ${scenario})
    set_tests_properties(file_move_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 目录创建事务：替代数据库边界，验证用户锁顺序、同名冲突和事务收尾。
add_executable(test_directory_creation
    test_directory_creation.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_creation.cpp
)
add_dependencies(test_directory_creation prepare_runtime_directories)
target_link_libraries(test_directory_creation PRIVATE common_lib)
foreach(scenario IN ITEMS invalid_arguments path_limits begin_failure existing_transaction
        user_results parent_results invalid_parents duplicate_file duplicate_directory
        insert_results committed root_parent quoted_name unknown_applied unknown_not_applied
        commit_not_active parent_moved parent_replaced exceptions repeat)
    add_test(NAME directory_creation_${scenario} COMMAND test_directory_creation ${scenario})
    set_tests_properties(directory_creation_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 空目录删除事务：替代数据库边界，验证根目录保护、判空和事务收尾。
add_executable(test_directory_deletion
    test_directory_deletion.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_deletion.cpp
)
add_dependencies(test_directory_deletion prepare_runtime_directories)
target_link_libraries(test_directory_deletion PRIVATE common_lib)
foreach(scenario IN ITEMS invalid_arguments path_limits begin_failure existing_transaction
        user_results parent_results target_results children_results nonempty_file
        nonempty_directory invalid_targets root_protection exact_path delete_results committed
        unknown_applied unknown_not_applied commit_not_active exceptions repeat)
    add_test(NAME directory_deletion_${scenario} COMMAND test_directory_deletion ${scenario})
    set_tests_properties(directory_deletion_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 递归目录删除：执行真实事务函数，替代数据库验证整棵子树的原子删除。
add_executable(test_directory_tree_deletion
    test_directory_tree_deletion.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_deletion.cpp
)
add_dependencies(test_directory_tree_deletion prepare_runtime_directories)
target_link_libraries(test_directory_tree_deletion PRIVATE common_lib)
foreach(scenario IN ITEMS invalid_arguments path_limits begin_failure existing_transaction
        user_results parent_results target_results invalid_targets exact_path children_results
        child_values duplicate_ids query_failure delete_failure delete_zero delete_many
        committed unknown_applied unknown_not_applied commit_not_active exceptions repeat
        empty_tree nested_tree wide_tree deep_tree literal_names root_child)
    add_test(NAME directory_tree_deletion_${scenario} COMMAND test_directory_tree_deletion ${scenario})
    set_tests_properties(directory_tree_deletion_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 子树协调：执行真实实现，不启动数据库、网络或存储服务。
add_executable(test_subtree_coordinator
    test_subtree_coordinator.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/subtree_coordinator.cpp
)
target_include_directories(test_subtree_coordinator PRIVATE ${PROJECT_SOURCE_DIR}/inc)
add_dependencies(test_subtree_coordinator prepare_runtime_directories)
target_link_libraries(test_subtree_coordinator PRIVATE common_lib Threads::Threads)
foreach(scenario IN ITEMS identity root descendants boundary unrelated literal_names long_paths component_matrix)
    add_test(NAME subtree_path_${scenario} COMMAND test_subtree_coordinator ${scenario})
    set_tests_properties(subtree_path_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 范围重叠：四种 exact/subtree 组合、方向对称性及独立目录树集合校验。
foreach(scenario IN ITEMS exact mixed subtrees boundary literal_names kind_effect tree_matrix)
    add_test(NAME subtree_scope_${scenario} COMMAND test_subtree_coordinator scope_${scenario})
    set_tests_properties(subtree_scope_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 请求冲突：用户隔离、读写组合、多范围配对及范围顺序不变性。
foreach(scenario IN ITEMS users readers access_modes scope_kinds multiple_scopes unrelated directory_delete permutations)
    add_test(NAME subtree_request_${scenario} COMMAND test_subtree_coordinator request_${scenario})
    set_tests_properties(subtree_request_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 请求入口校验：检查所有范围、枚举值与规范路径，不查询或修改文件系统。
foreach(scenario IN ITEMS empty_request access_mode scope_kind absolute_paths canonical_paths
        component_names path_limits later_scope valid_requests)
    add_test(NAME subtree_validation_${scenario} COMMAND test_subtree_coordinator validation_${scenario})
    set_tests_properties(subtree_validation_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 协调器运行时：真实线程、真实条件变量和 Guard；仅测试目标可观察内部队列。
add_executable(test_subtree_coordinator_runtime
    test_subtree_coordinator_runtime.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/subtree_coordinator.cpp
)
add_dependencies(test_subtree_coordinator_runtime prepare_runtime_directories)
target_compile_options(test_subtree_coordinator_runtime PRIVATE -fno-access-control)
target_link_libraries(test_subtree_coordinator_runtime PRIVATE common_lib Threads::Threads)
foreach(scenario IN ITEMS writer_reader writer_writer reader_writer readers users paths
        exact_descendant subtree_ancestor boundary subtree multiple_ranges reverse_order
        writer_priority unrelated_bypass reader_identity move_constructor move_assignment
        self_move empty_move destructor unwind cross_thread_release repeated_reset reallocation
        allocation_failures stress)
    add_test(NAME subtree_runtime_${scenario} COMMAND test_subtree_coordinator_runtime ${scenario})
    set_tests_properties(subtree_runtime_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 子树版本：祖先失效、保守碰撞、溢出降级和并发；不启动任何外部服务。
add_executable(test_subtree_versions
    test_subtree_versions.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/subtree_versions.cpp
)
target_include_directories(test_subtree_versions PRIVATE ${PROJECT_SOURCE_DIR}/inc)
target_compile_options(test_subtree_versions PRIVATE -fno-access-control)
target_link_libraries(test_subtree_versions PRIVATE Threads::Threads)
foreach(scenario IN ITEMS initial self root ancestors global_sequence ancestor_after_descendant
        descendant_isolation independent_namespaces collisions overflow disabled_advance
        no_allocation concurrent_updates snapshot_monotonic instances)
    add_test(NAME subtree_versions_${scenario} COMMAND test_subtree_versions ${scenario})
    set_tests_properties(subtree_versions_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 运行真实服务、缓存和删除事务；替代数据库边界验证失效、结果传播及并发读取。
add_executable(test_metadata_deletion
    test_metadata_deletion.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/metadata_service.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/file_deletion.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_creation.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/directory_deletion.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/upload_registration.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/object_repository.cpp
)
add_dependencies(test_metadata_deletion prepare_runtime_directories)
target_link_libraries(test_metadata_deletion PRIVATE common_lib Threads::Threads)
# 观察协调队列及分片，并直接验证目录查询私有方法的数据库结果校验。
target_compile_options(test_metadata_deletion PRIVATE -fno-access-control)

# 这些目标直接编译缓存及服务代码、使用各自的数据库替代实现，
# 因而单独加入子树协调和版本实现，不链接包含真实数据库调用的 metadata_lib。
foreach(cache_test IN ITEMS test_metadata_cache test_metadata_cache_pair
        test_metadata_deletion test_ftp_transfer)
    target_sources(${cache_test} PRIVATE
        ${PROJECT_SOURCE_DIR}/src/ftp/subtree_coordinator.cpp
        ${PROJECT_SOURCE_DIR}/src/ftp/subtree_versions.cpp
    )
endforeach()

# 服务实现调用重命名和移动事务；这些目标各自提供数据库替代实现。
# 纯缓存测试不调用服务层，因此无需加入该数据库事务实现。
foreach(service_test IN ITEMS test_metadata_cache test_metadata_deletion test_ftp_transfer)
    target_sources(${service_test} PRIVATE
        ${PROJECT_SOURCE_DIR}/src/ftp/file_rename.cpp
        ${PROJECT_SOURCE_DIR}/src/ftp/file_move.cpp
    )
endforeach()

foreach(scenario IN ITEMS committed unknown_applied unknown_not_applied borrow_failure
        begin_failure query_failure missing_file delete_failure delete_zero borrow_exception
        query_exception delete_exception invalid_arguments concurrent shared disabled)
    add_test(NAME metadata_deletion_${scenario} COMMAND test_metadata_deletion ${scenario})
    set_tests_properties(metadata_deletion_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 重命名参数必须在进入缓存写入和借用数据库连接之前验证完毕。
add_test(NAME metadata_rename_invalid_arguments COMMAND test_metadata_deletion rename_invalid_arguments)
set_tests_properties(metadata_rename_invalid_arguments PROPERTIES TIMEOUT 15)
foreach(scenario IN ITEMS committed unknown_applied unknown_not_applied borrow_failure begin_failure
        query_failure missing_source duplicate_file duplicate_directory update_failure update_zero update_many
        borrow_exception query_exception update_exception nonstandard_exception unchanged unchanged_missing
        shared disabled concurrent)
    add_test(NAME metadata_rename_${scenario} COMMAND test_metadata_deletion rename_${scenario})
    set_tests_properties(metadata_rename_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 移动服务使用真实双目录协调与事务，验证两个命名空间的缓存和三态结果。
foreach(scenario IN ITEMS invalid_arguments committed unknown_applied unknown_not_applied borrow_failure
        begin_failure query_failure missing_source duplicate_file duplicate_directory update_failure update_zero update_many
        borrow_exception query_exception update_exception nonstandard_exception same_directory same_directory_missing
        shared disabled concurrent_source concurrent_target root_source root_target gate_collision empty_target)
    add_test(NAME metadata_move_${scenario} COMMAND test_metadata_deletion move_${scenario})
    set_tests_properties(metadata_move_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 按完整路径查询目录：结果格式、用户/类型隔离、连接归还和异常传播。
foreach(scenario IN ITEMS invalid_arguments success root missing users file case_sensitive literal_names
        uncached borrow_failure borrow_exception query_exception nonstandard_exception query_failure
        multiple_rows no_columns extra_columns null_value empty_path wrong_path)
    add_test(NAME metadata_directory_lookup_${scenario}
        COMMAND test_metadata_deletion directory_lookup_${scenario})
    set_tests_properties(metadata_directory_lookup_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# cd 服务按规范目标共用缓存，并与目标父目录/祖先删除保持协调。
foreach(scenario IN ITEMS absolute relative parent dot root root_missing aliases invalid users missing file
        wrong_case target_invalidation ancestor_invalidation target_coordination source_independent
        query_failure malformed borrow_failure exception nonstandard_exception)
    add_test(NAME metadata_cd_service_${scenario} COMMAND test_metadata_deletion cd_service_${scenario})
    set_tests_properties(metadata_cd_service_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 同一服务层替代数据库还验证 mkdir 的三态返回、缓存失效与并发读取。
foreach(scenario IN ITEMS committed unknown_applied unknown_not_applied borrow_failure
        begin_failure query_failure insert_failure insert_zero borrow_exception query_exception
        insert_exception invalid_arguments concurrent shared disabled duplicate_file)
    add_test(NAME metadata_creation_${scenario} COMMAND test_metadata_deletion creation_${scenario})
    set_tests_properties(metadata_creation_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 空目录删除使用相同服务层替代数据库，验证父目录和目标目录的缓存范围。
foreach(scenario IN ITEMS committed not_empty_file not_empty_directory unknown_applied
        unknown_not_applied borrow_failure begin_failure query_failure delete_failure delete_zero
        borrow_exception query_exception delete_exception invalid_arguments concurrent shared disabled wrong_case)
    add_test(NAME metadata_rmdir_${scenario} COMMAND test_metadata_deletion rmdir_${scenario})
    set_tests_properties(metadata_rmdir_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

# 真实服务层调用真实递归事务；数据库替代实现只负责事务暂存与故障注入。
foreach(scenario IN ITEMS committed unknown_applied unknown_not_applied borrow_failure begin_failure
        query_failure missing_target wrong_case delete_failure delete_zero commit_not_active
        borrow_exception query_exception delete_exception nonstandard_exception invalid_arguments
        concurrent_parent concurrent_descendant shared disabled root_parent)
    add_test(NAME metadata_tree_${scenario} COMMAND test_metadata_deletion tree_${scenario})
    set_tests_properties(metadata_tree_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

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
