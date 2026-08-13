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

add_executable(m3-demo m3-demo.cpp)
target_link_libraries(m3-demo PRIVATE monitor_lib)

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
    add_monitor_test(monitor_callback_registry_test test/callback_registry.cpp)
    add_monitor_test(monitor_callback_test test/cb_test.cpp)
    add_monitor_test(monitor_engine_aio_test test/engine_aio_test.cpp)
    add_monitor_test(monitor_engine_loop_test test/engine_loop_test.cpp)
    add_monitor_test(monitor_engine_test test/engine_test.cpp)
    add_monitor_test(monitor_engine_timer_test test/engine_timer_test.cpp)
    add_monitor_test(monitor_header_split_test test/header_split_test.cpp)
    add_monitor_test(monitor_timer_manager_add_test test/timer_manager_add_test.cpp)
    add_monitor_test(monitor_timer_manager_check_test test/timer_manager_check_test.cpp)
    add_monitor_test(monitor_timer_manager_fixed_cadence_test test/timer_manager_fixed_cadence_test.cpp)
    add_monitor_test(monitor_timer_manager_lifecycle_test test/timer_manager_lifecycle_test.cpp)
    add_monitor_test(monitor_timer_types_test test/timer_types_test.cpp)
    add_monitor_test(monitor_wake_pipe_test test/wake_pipe_test.cpp)
    add_monitor_test(monitor_m3_smoke_test
        test/m3_smoke_test.cpp
        $<TARGET_FILE:m3-demo>
    )
    add_dependencies(monitor_m3_smoke_test m3-demo)
endif()
