add_library(common_lib STATIC
    common.cpp
    net.cpp
)

target_include_directories(common_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
)

target_link_libraries(common_lib PUBLIC
    Threads::Threads
    yaml-cpp
    logger_lib
)
