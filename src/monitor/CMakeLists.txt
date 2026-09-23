add_library(monitor_lib
    aio_manager.cpp
    alarm_protocol.cpp
    alarm_spool.cpp
    alarm_stream_decoder.cpp
    callback_registry.cpp
    cli_commands.cpp
    cli_prompt.cpp
    cli_registry.cpp
    cli_server.cpp
    engine.cpp
    monitor_collector.cpp
    monitor_error.cpp
    monitor_module_registry.cpp
    monitor_policy.cpp
    monitor_reporter.cpp
    monitor_store.cpp
    monitor_wire.cpp
    reliable_alarm_collector.cpp
    reliable_alarm_publisher.cpp
    timer_manager.cpp
    udp_publisher.cpp
    udp_receiver.cpp
    wake_pipe.cpp
)

target_include_directories(monitor_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/inc/monitor
)

target_link_libraries(monitor_lib PUBLIC
    Threads::Threads
)

add_executable(m3-demo demo/m3-demo.cpp)
target_link_libraries(m3-demo PRIVATE monitor_lib)

add_executable(demo_pub demo/demo_pub.cpp)
target_link_libraries(demo_pub PRIVATE monitor_lib)

add_executable(demo-m7 demo/demo-m7.cpp)
target_link_libraries(demo-m7 PRIVATE monitor_lib)

add_executable(demo-m8 demo/demo-m8.cpp)
target_link_libraries(demo-m8 PRIVATE monitor_lib)

add_executable(demo-m9-collector
    demo/demo-m9-collector.cpp
    demo/demo_modules.cpp
)
target_link_libraries(demo-m9-collector PRIVATE monitor_lib)

add_executable(demo-m9-node
    demo/demo-m9-node.cpp
    demo/demo_modules.cpp
)
target_link_libraries(demo-m9-node PRIVATE monitor_lib)

add_executable(demo-m9-policy demo/demo-m9-policy.cpp)
target_link_libraries(demo-m9-policy PRIVATE monitor_lib)

if(BUILD_TESTS)
    function(add_monitor_test target source)
        add_executable(${target} ${source})
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test"
        )
        target_link_libraries(${target} PRIVATE monitor_lib)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-UNDEBUG>
        )
        add_test(NAME ${target} COMMAND ${target} ${ARGN})
    endfunction()

    add_monitor_test(monitor_aio_manager_test test/aio_manager_test.cpp)
    add_monitor_test(monitor_alarm_cli_status_test test/alarm_cli_status_test.cpp)
    add_monitor_test(monitor_alarm_protocol_contract_test test/alarm_protocol_contract_test.cpp)
    add_monitor_test(monitor_alarm_protocol_test test/alarm_protocol_test.cpp)
    add_monitor_test(monitor_alarm_spool_test test/alarm_spool_test.cpp)
    add_monitor_test(monitor_alarm_stream_decoder_test test/alarm_stream_decoder_test.cpp)
    add_monitor_test(monitor_callback_registry_test test/callback_registry.cpp)
    add_monitor_test(monitor_callback_test test/cb_test.cpp)
    add_monitor_test(monitor_cli_commands_test test/cli_commands_test.cpp)
    add_monitor_test(monitor_cli_db_dump_test test/cli_db_dump_test.cpp)
    add_monitor_test(monitor_cli_error_metadata_test test/cli_error_metadata_test.cpp)
    add_monitor_test(monitor_cli_module_server_test test/cli_module_server_test.cpp)
    add_monitor_test(monitor_cli_prompt_test test/cli_prompt_test.cpp)
    add_monitor_test(monitor_cli_registry_session_test test/cli_registry_session_test.cpp)
    add_monitor_test(monitor_cli_registry_test test/cli_registry_test.cpp)
    add_monitor_test(monitor_cli_server_session_test test/cli_server_session_test.cpp)
    add_monitor_test(monitor_cli_server_test test/cli_server_test.cpp)
    add_monitor_test(monitor_cli_session_context_test test/cli_session_context_test.cpp)
    add_monitor_test(monitor_cli_types_test test/cli_types_test.cpp)
    add_monitor_test(monitor_engine_aio_test test/engine_aio_test.cpp)
    add_monitor_test(monitor_engine_cli_test test/monitor_engine_cli_test.cpp)
    add_monitor_test(monitor_engine_loop_test test/engine_loop_test.cpp)
    add_monitor_test(monitor_engine_test test/engine_test.cpp)
    add_monitor_test(monitor_engine_timer_test test/engine_timer_test.cpp)
    add_monitor_test(monitor_header_split_test test/header_split_test.cpp)
    add_monitor_test(monitor_engine_m5_compat_test test/monitor_engine_m5_compat_test.cpp)
    add_monitor_test(monitor_engine_metadata_wire_test test/monitor_engine_metadata_wire_test.cpp)
    add_monitor_test(monitor_engine_module_test test/monitor_engine_module_test.cpp)
    add_monitor_test(monitor_engine_reporter_test test/monitor_engine_reporter_test.cpp)
    add_monitor_test(monitor_engine_reporter_stage8_test test/monitor_engine_reporter_stage8_test.cpp)
    add_monitor_test(monitor_engine_reporter_stage9_test test/monitor_engine_reporter_stage9_test.cpp)
    add_monitor_test(monitor_engine_udp_publisher_test test/monitor_engine_udp_publisher_test.cpp)
    add_monitor_test(monitor_engine_udp_receiver_aio_test test/monitor_engine_udp_receiver_aio_test.cpp)
    add_monitor_test(monitor_module_registry_test test/monitor_module_registry_test.cpp)
    add_monitor_test(monitor_module_error_registry_test test/monitor_module_error_registry_test.cpp)
    add_monitor_test(monitor_module_types_test test/monitor_module_types_test.cpp)
    add_monitor_test(monitor_error_types_test test/monitor_error_types_test.cpp)
    add_monitor_test(monitor_metadata_concurrency_test test/monitor_metadata_concurrency_test.cpp)
    add_monitor_test(monitor_policy_cont_for_test test/monitor_policy_cont_for_test.cpp)
    add_monitor_test(monitor_policy_contract_test test/monitor_policy_contract_test.cpp)
    add_monitor_test(monitor_policy_engine_integration_test test/monitor_policy_engine_integration_test.cpp)
    add_monitor_test(monitor_policy_smooth_trim_test test/monitor_policy_smooth_trim_test.cpp)
    add_monitor_test(monitor_policy_times_per_period_test test/monitor_policy_times_per_period_test.cpp)
    add_monitor_test(monitor_reliable_alarm_routing_test test/monitor_reliable_alarm_routing_test.cpp)
    add_monitor_test(monitor_reporter_contract_test test/monitor_reporter_contract_test.cpp)
    add_monitor_test(monitor_reporter_count_test test/monitor_reporter_count_test.cpp)
    add_monitor_test(monitor_reporter_dedup_order_test test/monitor_reporter_dedup_order_test.cpp)
    add_monitor_test(monitor_reporter_error_test test/monitor_reporter_error_test.cpp)
    add_monitor_test(monitor_reporter_enrichment_test test/monitor_reporter_enrichment_test.cpp)
    add_monitor_test(monitor_reporter_publisher_concurrency_test test/monitor_reporter_publisher_concurrency_test.cpp)
    add_monitor_test(monitor_reporter_string_test test/monitor_reporter_string_test.cpp)
    add_monitor_test(monitor_engine_store_test test/monitor_engine_store_test.cpp)
    add_monitor_test(monitor_store_basic_test test/monitor_store_basic_test.cpp)
    add_monitor_test(monitor_store_concurrency_test test/monitor_store_concurrency_test.cpp)
    add_monitor_test(monitor_store_numeric_test test/monitor_store_numeric_test.cpp)
    add_monitor_test(monitor_store_prepared_update_test test/monitor_store_prepared_update_test.cpp)
    add_monitor_test(monitor_store_query_test test/monitor_store_query_test.cpp)
    add_monitor_test(monitor_store_string_test test/monitor_store_string_test.cpp)
    add_monitor_test(monitor_udp_publisher_test test/udp_publisher_test.cpp)
    add_monitor_test(monitor_udp_publisher_concurrency_test test/udp_publisher_concurrency_test.cpp)
    add_monitor_test(monitor_wire_contract_test test/monitor_wire_contract_test.cpp)
    add_monitor_test(monitor_wire_endian_test test/monitor_wire_endian_test.cpp)
    add_monitor_test(monitor_wire_v1_test test/monitor_wire_v1_test.cpp)
    add_monitor_test(monitor_wire_v2_header_test test/monitor_wire_v2_header_test.cpp)
    add_monitor_test(monitor_wire_v2_numeric_test test/monitor_wire_v2_numeric_test.cpp)
    add_monitor_test(monitor_wire_v2_string_test test/monitor_wire_v2_string_test.cpp)
    add_monitor_test(monitor_timer_manager_add_test test/timer_manager_add_test.cpp)
    add_monitor_test(monitor_timer_manager_check_test test/timer_manager_check_test.cpp)
    add_monitor_test(monitor_timer_manager_fixed_cadence_test test/timer_manager_fixed_cadence_test.cpp)
    add_monitor_test(monitor_timer_manager_lifecycle_test test/timer_manager_lifecycle_test.cpp)
    add_monitor_test(monitor_timer_types_test test/timer_types_test.cpp)
    add_monitor_test(monitor_reliable_alarm_collector_test test/reliable_alarm_collector_test.cpp)
    add_monitor_test(monitor_reliable_alarm_publisher_test test/reliable_alarm_publisher_test.cpp)
    add_monitor_test(monitor_reliable_alarm_transport_test test/reliable_alarm_transport_test.cpp)
    add_monitor_test(monitor_wake_pipe_test test/wake_pipe_test.cpp)
    add_monitor_test(monitor_m3_smoke_test
        test/m3_smoke_test.cpp
        $<TARGET_FILE:m3-demo>
    )
    add_dependencies(monitor_m3_smoke_test m3-demo)
    add_monitor_test(monitor_m7_demo_smoke_test
        test/m7_demo_smoke_test.cpp
        $<TARGET_FILE:demo-m7>
    )
    add_dependencies(monitor_m7_demo_smoke_test demo-m7)
    add_monitor_test(monitor_m8_demo_smoke_test
        test/m8_demo_smoke_test.cpp
        $<TARGET_FILE:demo-m8>
    )
    add_dependencies(monitor_m8_demo_smoke_test demo-m8)
    set_tests_properties(
        monitor_m3_smoke_test
        monitor_m7_demo_smoke_test
        PROPERTIES RESOURCE_LOCK monitor_demo_port_9000
    )
    set_tests_properties(
        monitor_m8_demo_smoke_test
        PROPERTIES RESOURCE_LOCK monitor_demo_port_9001
    )
endif()
