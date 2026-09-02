#!/usr/bin/env bash

set -u
set -o pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

cxx=${CXX:-g++}
asan_options=detect_leaks=1:halt_on_error=1:abort_on_error=1
ubsan_options=halt_on_error=1:print_stacktrace=1

compiler_flags=(
    -std=c++17
    -O1
    -g
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
    -pthread
)

production_sources=(
    "$project_root/callback_registry.cpp"
    "$project_root/wake_pipe.cpp"
    "$project_root/aio_manager.cpp"
    "$project_root/timer_manager.cpp"
    "$project_root/monitor_store.cpp"
    "$project_root/monitor_reporter.cpp"
    "$project_root/monitor_wire.cpp"
    "$project_root/engine.cpp"
    "$project_root/udp_publisher.cpp"
    "$project_root/udp_receiver.cpp"
)

wire_tests=(
    monitor_wire_contract_test
    monitor_wire_endian_test
    monitor_wire_v1_test
    monitor_wire_v2_header_test
    monitor_wire_v2_numeric_test
    monitor_wire_v2_string_test
)

asan_temp_dir=$(mktemp -d /tmp/udp_publisher_concurrency_asan.XXXXXX)
trap 'rm -rf -- "$asan_temp_dir"' EXIT

if ! command -v "$cxx" >/dev/null 2>&1; then
    printf 'udp_publisher_concurrency_asan=COMPILER_NOT_FOUND compiler=%s\n' \
        "$cxx" >&2
    exit 1
fi

if ! command -v timeout >/dev/null 2>&1; then
    printf '%s\n' \
        'udp_publisher_concurrency_asan=TIMEOUT_COMMAND_NOT_FOUND' >&2
    exit 1
fi

concurrency_binary="$asan_temp_dir/concurrency_test"
if ! "$cxx" "${compiler_flags[@]}" \
    "${production_sources[@]}" \
    "$project_root/test/udp_publisher_concurrency_test.cpp" \
    -o "$concurrency_binary"; then
    printf '%s\n' 'udp_publisher_concurrency_asan=COMPILE_FAIL' >&2
    exit 1
fi

ASAN_OPTIONS="$asan_options" \
UBSAN_OPTIONS="$ubsan_options" \
    timeout --kill-after=10s 120s "$concurrency_binary"
concurrency_status=$?

if [ "$concurrency_status" -ne 0 ]; then
    printf \
        'udp_publisher_concurrency_asan=FAIL exit=%d\n' \
        "$concurrency_status" >&2
    exit "$concurrency_status"
fi

for test_name in "${wire_tests[@]}"; do
    wire_binary="$asan_temp_dir/${test_name}"

    if ! "$cxx" "${compiler_flags[@]}" \
        "$project_root/monitor_wire.cpp" \
        "$project_root/test/${test_name}.cpp" \
        -o "$wire_binary"; then
        printf \
            'monitor_wire_asan=COMPILE_FAIL test=%s\n' \
            "$test_name" >&2
        exit 1
    fi

    ASAN_OPTIONS="$asan_options" \
    UBSAN_OPTIONS="$ubsan_options" \
        timeout --kill-after=5s 30s "$wire_binary"
    wire_status=$?

    if [ "$wire_status" -ne 0 ]; then
        printf \
            'monitor_wire_asan=FAIL test=%s exit=%d\n' \
            "$test_name" \
            "$wire_status" >&2
        exit "$wire_status"
    fi
done

printf '%s\n' 'udp_publisher_concurrency_asan=PASS'
printf 'monitor_wire_asan=PASS tests=%d\n' "${#wire_tests[@]}"
