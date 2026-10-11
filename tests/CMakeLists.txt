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
