add_library(monitor_lib STATIC
    aio_manager.cpp
    callback_registry.cpp
    engine.cpp
    timer_manager.cpp
    wake_pipe.cpp
)

target_include_directories(monitor_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/inc/monitor
)

target_link_libraries(monitor_lib PUBLIC
    Threads::Threads
)
