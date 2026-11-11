set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test")
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

add_executable(test_storage_engines test_storage_engines.cpp)
add_dependencies(test_storage_engines prepare_runtime_directories)
target_link_libraries(test_storage_engines PRIVATE transfer_lib)
add_test(NAME storage_engines_test COMMAND test_storage_engines)

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
)
add_dependencies(test_metadata_cache prepare_runtime_directories)
target_link_libraries(test_metadata_cache PRIVATE common_lib OpenSSL::Crypto)
foreach(scenario IN ITEMS list cd download failures writes failed_write concurrent stale_fill
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
)
add_dependencies(test_ftp_transfer prepare_runtime_directories)
target_compile_options(test_ftp_transfer PRIVATE -fno-access-control)
target_link_libraries(test_ftp_transfer PRIVATE transfer_lib common_lib OpenSSL::Crypto)
foreach(scenario IN ITEMS
        commit_hash upload download error_frame busy_reply receive_error
        control_limit source_type source_fifo upload_chunked client_download shutdown_wait
        cold_send source_cancel stale_read source_eof client_upload empty_send reader_error legacy_send
        metadata_failure stale_prepare upload_cancel task_isolation
        metrics_auth metrics_snapshots metrics_stale_commit metrics_pool metrics_storage)
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
