add_subdirectory(memory)

option(TLSS_BUILD_MEMCPY_GLIBC_BENCHMARK "Build the TLSS versus glibc memcpy benchmark" OFF)
if(TLSS_BUILD_MEMCPY_GLIBC_BENCHMARK)
    add_subdirectory(benchmark)
endif()

add_library(common_lib
    timestamp.cpp
    common.cpp
    net.cpp
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
