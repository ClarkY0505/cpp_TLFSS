#如果只是编辑已有目标已经包含的 .h/.cpp，不需要重新 cmake --preset debug，直接构建即可：
# cmake --build --preset debug
# 如果是新增 .cpp 文件，通常需要把它加入对应的 CMakeLists.txt，然后重新配置一次：
# cmake --preset debug
# cmake --build --preset debug


cmake_minimum_required(VERSION 3.16)
project(cpp-toy-level-storage-service VERSION 1.0.0 LANGUAGES CXX)

# C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wpedantic)

# 构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# =============================================================================
# 查找依赖
# =============================================================================

# pthread
find_package(Threads REQUIRED)

# OpenSSL
find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")
endif()

# Protobuf — use CONFIG mode to avoid FindProtobuf version mismatch warning
# (protoc reports release version e.g. 31.1, libprotobuf reports ABI version e.g. 6.31.1)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
if(Protobuf_FOUND)
    message(STATUS "Protobuf found: ${Protobuf_VERSION}")
endif()

# gRPC
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    # 尝试使用 pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED grpc++ grpc)
    set(GRPC_LIBRARIES ${GRPC_LIBRARIES})
    set(GRPC_INCLUDE_DIRS ${GRPC_INCLUDE_DIRS})
else()
    message(STATUS "gRPC found via CMake config")
    set(GRPC_LIBRARIES gRPC::grpc++ gRPC::grpc)
endif()

# yaml-cpp
find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(YAML_CPP REQUIRED yaml-cpp)
else()
    message(STATUS "yaml-cpp found: ${yaml-cpp_VERSION}")
endif()

# GTest (可选)
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        message(STATUS "GTest found")
        enable_testing()
    else()
        message(STATUS "GTest not found, tests will be skipped")
        set(BUILD_TESTS OFF)
    endif()
endif()

# =============================================================================
# Proto 文件编译
# =============================================================================

# 查找 protoc 和 grpc_cpp_plugin
find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

if(NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR "protoc not found")
endif()

if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
endif()

message(STATUS "protoc: ${PROTOC_EXECUTABLE}")
message(STATUS "grpc_cpp_plugin: ${GRPC_CPP_PLUGIN}")

# Proto 文件目录
set(PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

# Proto 文件列表
set(PROTO_FILES
    ${PROTO_DIR}/storage_service.proto
    ${PROTO_DIR}/auth_service.proto
    ${PROTO_DIR}/monitor_service.proto
    ${PROTO_DIR}/gateway_service.proto
)

# 生成的文件列表
set(PROTO_SRCS)
set(PROTO_HDRS)
set(GRPC_SRCS)
set(GRPC_HDRS)

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    
    list(APPEND PROTO_SRCS ${GENERATED_DIR}/${PROTO_NAME}.pb.cc)
    list(APPEND PROTO_HDRS ${GENERATED_DIR}/${PROTO_NAME}.pb.h)
    list(APPEND GRPC_SRCS ${GENERATED_DIR}/${PROTO_NAME}.grpc.pb.cc)
    list(APPEND GRPC_HDRS ${GENERATED_DIR}/${PROTO_NAME}.grpc.pb.h)
endforeach()

# 自定义命令编译 proto 文件
add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
        --proto_path=${PROTO_DIR}
        --cpp_out=${GENERATED_DIR}
        --grpc_out=${GENERATED_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Generating protobuf and gRPC sources"
    VERBATIM
)

# Proto 库
add_library(proto_lib STATIC
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_include_directories(proto_lib PUBLIC
    ${GENERATED_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(proto_lib PUBLIC
    protobuf::libprotobuf
    ${GRPC_LIBRARIES}
)

# =============================================================================
# 包含目录
# =============================================================================

include_directories(
    ${CMAKE_SOURCE_DIR}/inc
    ${CMAKE_SOURCE_DIR}/third
    ${GENERATED_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

# =============================================================================
# 子模块
# =============================================================================

add_subdirectory(src/common)
add_subdirectory(src/ftp)
add_subdirectory(src/gateway)
add_subdirectory(src/logger)
add_subdirectory(src/monitor)
add_subdirectory(src/rpc)

# =============================================================================
# 主程序
# =============================================================================

add_executable(storage-service src/main.cpp)

target_link_libraries(storage-service
    common_lib
    ftp_lib
    gateway_lib
    logger_lib
    monitor_lib
    rpc_lib
    proto_lib
    ${OPENSSL_LIBRARIES}
    yaml-cpp
    Threads::Threads
)

# =============================================================================
# 编译完成后拷贝到项目根目录（方案 2）
# =============================================================================

add_custom_target(copy_bin_to_root ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}   # bin 目录
        ${CMAKE_SOURCE_DIR}/bin             # 拷贝到项目根目录下 bin
    DEPENDS storage-service                # 依赖可执行文件，确保先编译
    COMMENT "Copy entire bin directory to project root"
)

# =============================================================================
# 测试
# =============================================================================

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# =============================================================================
# 安装
# =============================================================================

install(TARGETS storage-service RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/storage-service)

# =============================================================================
# 打印配置信息
# =============================================================================

message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Build tests: ${BUILD_TESTS}")
message(STATUS "===========================")

