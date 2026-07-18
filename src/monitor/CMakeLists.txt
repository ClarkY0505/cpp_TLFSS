add_library(monitor_lib STATIC
    monitoringSys.c
    monitoringTimer.c
)
target_link_libraries(monitor_lib PUBLIC common_lib rpc_lib proto_lib)
add_executable(monitor demo.c)
target_link_libraries(monitor PRIVATE monitor_lib)
