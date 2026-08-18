#!/usr/bin/env bash

set -u
set -o pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)

asan_runs=${ASAN_RUNS:-20}
asan_run=1
asan_timeout_seconds=${ASAN_TIMEOUT_SECONDS:-60}
asan_kill_after_seconds=${ASAN_KILL_AFTER_SECONDS:-5}
cxx=${CXX:-g++}

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
    -I"$project_root/inc/monitor"
)

if ! command -v "$cxx" >/dev/null 2>&1; then
    printf 'monitor_store_concurrency_asan=COMPILER_NOT_FOUND compiler=%s\n' "$cxx" >&2
    exit 1
fi

if ! command -v timeout >/dev/null 2>&1; then
    printf '%s\n' 'monitor_store_concurrency_asan=TIMEOUT_COMMAND_NOT_FOUND' >&2
    exit 1
fi

asan_binary=$(mktemp /tmp/monitor_store_concurrency_asan.XXXXXX)
trap 'rm -f -- "$asan_binary"' EXIT

if ! "$cxx" "${compiler_flags[@]}" \
    "$project_root/src/monitor/monitor_store.cpp" \
    "$script_dir/monitor_store_concurrency_test.cpp" \
    -o "$asan_binary"; then

    printf '%s\n' 'monitor_store_concurrency_asan=COMPILE_FAIL' >&2
    exit 1
fi

while [ "$asan_run" -le "$asan_runs" ]; do
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        timeout \
            --kill-after="${asan_kill_after_seconds}s" \
            "${asan_timeout_seconds}s" \
            "$asan_binary"

    asan_status=$?

    if [ "$asan_status" -ne 0 ]; then
        if [ "$asan_status" -eq 124 ]; then
            printf \
                'monitor_store_concurrency_asan=TIMEOUT run=%d exit=%d\n' \
                "$asan_run" \
                "$asan_status" >&2
        else
            printf \
                'monitor_store_concurrency_asan=FAIL run=%d exit=%d\n' \
                "$asan_run" \
                "$asan_status" >&2
        fi

        exit "$asan_status"
    fi

    asan_run=$((asan_run + 1))
done

printf \
    'monitor_store_concurrency_asan=PASS runs=%d\n' \
    "$asan_runs"
