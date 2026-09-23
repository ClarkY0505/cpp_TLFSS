add_executable(test_tlss_memory_link test_tlss_memory_link.cpp)
target_link_libraries(test_tlss_memory_link PRIVATE common_lib)
add_test(NAME tlss_memory_link_test COMMAND test_tlss_memory_link)

if(GTest_FOUND)
    add_executable(test_ftp_server test_ftp_server.cpp)
    target_link_libraries(test_ftp_server
        ftp_lib
        common_lib
        GTest::GTest
        GTest::Main
    )
    add_test(NAME ftp_server_test COMMAND test_ftp_server)

    add_executable(test_monitor_timer test_monitor_timer.cpp)
    target_link_libraries(test_monitor_timer
        monitor_lib
        GTest::GTest
        GTest::Main
    )
    add_test(NAME monitor_timer_test COMMAND test_monitor_timer)
endif()
