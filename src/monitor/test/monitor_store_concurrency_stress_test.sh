#!/usr/bin/env bash

set -u
set -o pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)

stress_runs=${STRESS_RUNS:-200}
stress_run=1
stress_timeout_seconds=${STRESS_TIMEOUT_SECONDS:-10}
stress_kill_after_seconds=${STRESS_KILL_AFTER_SECONDS:-2}
cxx=${CXX:-g++}

compiler_flags=(
    -std=c++17
    -O2
    -g
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -pthread
    -I"$project_root/inc/monitor"
)

if ! command -v "$cxx" >/dev/null 2>&1; then
    printf 'monitor_store_concurrency_stress=COMPILER_NOT_FOUND compiler=%s\n' "$cxx" >&2
    exit 1
fi

if ! command -v timeout >/dev/null 2>&1; then
    printf '%s\n' 'monitor_store_concurrency_stress=TIMEOUT_COMMAND_NOT_FOUND' >&2
    exit 1
fi

stress_binary=$(mktemp /tmp/monitor_store_concurrency_stress.XXXXXX)
trap 'rm -f -- "$stress_binary"' EXIT

if ! "$cxx" "${compiler_flags[@]}" \
    "$project_root/src/monitor/monitor_store.cpp" \
    "$script_dir/monitor_store_concurrency_test.cpp" \
    -o "$stress_binary"; then

    printf '%s\n' 'monitor_store_concurrency_stress=COMPILE_FAIL' >&2
    exit 1
fi

while [ "$stress_run" -le "$stress_runs" ]; do
    timeout \
        --kill-after="${stress_kill_after_seconds}s" \
        "${stress_timeout_seconds}s" \
        "$stress_binary"

    stress_status=$?

    if [ "$stress_status" -ne 0 ]; then
        if [ "$stress_status" -eq 124 ]; then
            printf \
                'monitor_store_concurrency_stress=TIMEOUT run=%d exit=%d\n' \
                "$stress_run" \
                "$stress_status" >&2
        else
            printf \
                'monitor_store_concurrency_stress=FAIL run=%d exit=%d\n' \
                "$stress_run" \
                "$stress_status" >&2
        fi

        exit "$stress_status"
    fi

    stress_run=$((stress_run + 1))
done

printf \
    'monitor_store_concurrency_stress=PASS runs=%d\n' \
    "$stress_runs"
