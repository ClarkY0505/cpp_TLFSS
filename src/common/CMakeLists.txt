add_subdirectory(memory)

option(TLSS_BUILD_MEMCPY_GLIBC_BENCHMARK "Build the TLSS versus glibc memcpy benchmark" OFF)
if(TLSS_BUILD_MEMCPY_GLIBC_BENCHMARK)
    add_subdirectory(benchmark)
endif()

add_library(common_lib
    base/current_thread.cpp
    base/tlss_thread.cpp
    base/tlss_thread_pool.cpp
    base/mysql_connection.cpp
    base/mysql_connection_pool.cpp
    net/timestamp.cpp
    net/inet_address.cpp
    net/acceptor.cpp
    net/buffer.cpp
    net/channel.cpp
    net/event_loop.cpp
    net/event_loop_thread.cpp
    net/event_loop_thread_pool.cpp
    net/poller.cpp
    net/dafault_poller.cpp
    net/epoll_poller.cpp
    net/socket.cpp
    net/tcp_server.cpp
    net/tcp_connection.cpp
)

target_include_directories(common_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
)

target_link_libraries(common_lib PUBLIC
    tlss_memory
    Threads::Threads
    yaml-cpp
    logger_lib
)

target_link_libraries(common_lib PRIVATE MySQLClient::MySQLClient)
