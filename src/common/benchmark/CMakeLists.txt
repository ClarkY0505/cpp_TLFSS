add_executable(memcpy_glibc_benchmark memcpy_glibc_benchmark.cpp)
add_dependencies(memcpy_glibc_benchmark prepare_runtime_directories)

target_compile_features(memcpy_glibc_benchmark PRIVATE cxx_std_17)
target_compile_options(memcpy_glibc_benchmark PRIVATE
    -O3
    -march=native
    -fno-builtin-memcpy
    -fno-omit-frame-pointer
)
target_link_libraries(memcpy_glibc_benchmark PRIVATE tlss_memory Threads::Threads)
