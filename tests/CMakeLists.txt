set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test")
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

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

# Compile the real FTP handlers with local database/authentication substitutes.
# Private access is limited to this regression target for callback/error injection.
add_executable(test_ftp_transfer
    test_ftp_transfer.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/server.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/service_engine.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/client.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/async_file_receiver.cpp
    ${PROJECT_SOURCE_DIR}/src/ftp/async_file_source.cpp
)
add_dependencies(test_ftp_transfer prepare_runtime_directories)
target_compile_options(test_ftp_transfer PRIVATE -fno-access-control)
target_link_libraries(test_ftp_transfer PRIVATE common_lib OpenSSL::Crypto)
foreach(scenario IN ITEMS
        commit_hash upload download error_frame busy_reply receive_error
        control_limit source_type source_fifo upload_chunked client_download shutdown_wait
        cold_send source_cancel stale_read source_eof client_upload empty_send reader_error legacy_send)
    add_test(NAME ftp_transfer_${scenario} COMMAND test_ftp_transfer ${scenario})
    set_tests_properties(ftp_transfer_${scenario} PROPERTIES TIMEOUT 15)
endforeach()

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
