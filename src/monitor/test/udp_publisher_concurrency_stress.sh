#!/usr/bin/env bash

set -u
set -o pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

cxx=${CXX:-g++}
stress_runs=${M6_STAGE11_STRESS_RUNS:-100}
stress_run=1

compiler_flags=(
    -std=c++17
    -O2
    -g
    -Wall
    -Wextra
    -Wpedantic
    -Werror
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

if [[ ! "$stress_runs" =~ ^[1-9][0-9]*$ ]] ||
   [ "${#stress_runs}" -gt 5 ] ||
   [ "$stress_runs" -gt 10000 ]; then
    printf \
        'udp_publisher_concurrency_stress=INVALID_RUNS value=%s range=1..10000\n' \
        "$stress_runs" >&2
    exit 2
fi

stress_temp_dir=$(mktemp -d /tmp/udp_publisher_concurrency_stress.XXXXXX)
stress_binary="$stress_temp_dir/test"
trap 'rm -rf -- "$stress_temp_dir"' EXIT

if ! command -v "$cxx" >/dev/null 2>&1; then
    printf 'udp_publisher_concurrency_stress=COMPILER_NOT_FOUND compiler=%s\n' \
        "$cxx" >&2
    exit 1
fi

if ! command -v timeout >/dev/null 2>&1; then
    printf '%s\n' \
        'udp_publisher_concurrency_stress=TIMEOUT_COMMAND_NOT_FOUND' >&2
    exit 1
fi

if ! "$cxx" "${compiler_flags[@]}" \
    "${production_sources[@]}" \
    "$project_root/test/udp_publisher_concurrency_test.cpp" \
    -o "$stress_binary"; then
    printf '%s\n' 'udp_publisher_concurrency_stress=COMPILE_FAIL' >&2
    exit 1
fi

while [ "$stress_run" -le "$stress_runs" ]; do
    timeout --kill-after=5s 30s "$stress_binary"
    stress_status=$?

    if [ "$stress_status" -ne 0 ]; then
        if [ "$stress_status" -eq 124 ]; then
            printf \
                'udp_publisher_concurrency_stress=TIMEOUT run=%d exit=%d\n' \
                "$stress_run" \
                "$stress_status" >&2
        else
            printf \
                'udp_publisher_concurrency_stress=FAIL run=%d exit=%d\n' \
                "$stress_run" \
                "$stress_status" >&2
        fi

        exit "$stress_status"
    fi

    stress_run=$((stress_run + 1))
done

printf 'udp_publisher_concurrency_stress=PASS runs=%d\n' "$stress_runs"
