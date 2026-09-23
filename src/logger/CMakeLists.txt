add_library(logger_lib logger.cpp)
target_include_directories(logger_lib PUBLIC ${CMAKE_SOURCE_DIR}/third)
target_link_libraries(logger_lib PUBLIC Threads::Threads)
