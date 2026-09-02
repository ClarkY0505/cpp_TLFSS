#!/usr/bin/env bash

set -u
set -o pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

cxx=${CXX:-g++}
tsan_runs=${M6_STAGE11_TSAN_RUNS:-10}
tsan_run=1
tsan_options=halt_on_error=1:exitcode=66

compiler_flags=(
    -std=c++17
    -O1
    -g
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -fsanitize=thread
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

if [[ ! "$tsan_runs" =~ ^[1-9][0-9]*$ ]] ||
   [ "${#tsan_runs}" -gt 5 ] ||
   [ "$tsan_runs" -gt 10000 ]; then
    printf \
        'udp_publisher_concurrency_tsan=INVALID_RUNS value=%s range=1..10000\n' \
        "$tsan_runs" >&2
    exit 2
fi

tsan_temp_dir=$(mktemp -d /tmp/udp_publisher_concurrency_tsan.XXXXXX)
tsan_binary="$tsan_temp_dir/test"
tsan_probe_binary="$tsan_temp_dir/probe"
trap 'rm -rf -- "$tsan_temp_dir"' EXIT

if ! command -v "$cxx" >/dev/null 2>&1; then
    printf 'udp_publisher_concurrency_tsan=COMPILER_NOT_FOUND compiler=%s\n' \
        "$cxx" >&2
    exit 1
fi

if ! command -v timeout >/dev/null 2>&1; then
    printf '%s\n' \
        'udp_publisher_concurrency_tsan=TIMEOUT_COMMAND_NOT_FOUND' >&2
    exit 1
fi

# 先探测当前环境能否启动 TSan runtime。
# 某些启用了高地址随机化的环境会在项目代码运行前报
# "ThreadSanitizer: unexpected memory mapping"。这种情况下尝试使用
# setarch -R 仅对测试进程关闭 ASLR，不修改系统全局配置。
if ! printf '%s\n' 'int main() { return 0; }' \
    | "$cxx" "${compiler_flags[@]}" -x c++ - \
        -o "$tsan_probe_binary"; then
    printf '%s\n' 'udp_publisher_concurrency_tsan=PROBE_COMPILE_FAIL' >&2
    exit 1
fi

runtime_prefix=()
probe_output=$(TSAN_OPTIONS="$tsan_options" "$tsan_probe_binary" 2>&1)
probe_status=$?

if [ "$probe_status" -ne 0 ]; then
    if [[ "$probe_output" == *"unexpected memory mapping"* ]] && \
       command -v setarch >/dev/null 2>&1; then
        machine_arch=$(uname -m)
        fallback_output=$(
            TSAN_OPTIONS="$tsan_options" \
                setarch "$machine_arch" -R "$tsan_probe_binary" 2>&1
        )
        fallback_status=$?

        if [ "$fallback_status" -eq 0 ]; then
            runtime_prefix=(setarch "$machine_arch" -R)
            printf \
                'udp_publisher_concurrency_tsan=TSAN_RUNTIME_FALLBACK mode=setarch-%s-R\n' \
                "$machine_arch"
        else
            printf '%s\n' "$probe_output" >&2
            printf '%s\n' "$fallback_output" >&2
            printf \
                'udp_publisher_concurrency_tsan=ENVIRONMENT_UNSUPPORTED exit=%d\n' \
                "$fallback_status" >&2
            exit 77
        fi
    else
        printf '%s\n' "$probe_output" >&2
        printf \
            'udp_publisher_concurrency_tsan=TSAN_RUNTIME_FAIL exit=%d\n' \
            "$probe_status" >&2
        exit 77
    fi
fi

if ! "$cxx" "${compiler_flags[@]}" \
    "${production_sources[@]}" \
    "$project_root/test/udp_publisher_concurrency_test.cpp" \
    -o "$tsan_binary"; then
    printf '%s\n' 'udp_publisher_concurrency_tsan=COMPILE_FAIL' >&2
    exit 1
fi

while [ "$tsan_run" -le "$tsan_runs" ]; do
    tsan_output=$(
        TSAN_OPTIONS="$tsan_options" \
            timeout --kill-after=10s 120s \
            "${runtime_prefix[@]}" \
            "$tsan_binary" 2>&1
    )
    tsan_status=$?

    if [ "$tsan_status" -ne 0 ] && \
       [ "${#runtime_prefix[@]}" -eq 0 ] && \
       [[ "$tsan_output" == *"unexpected memory mapping"* ]] && \
       command -v setarch >/dev/null 2>&1; then
        machine_arch=$(uname -m)
        tsan_output=$(
            TSAN_OPTIONS="$tsan_options" \
                timeout --kill-after=10s 120s \
                setarch "$machine_arch" -R \
                "$tsan_binary" 2>&1
        )
        tsan_status=$?

        if [ "$tsan_status" -eq 0 ]; then
            runtime_prefix=(setarch "$machine_arch" -R)
            printf \
                'udp_publisher_concurrency_tsan=TSAN_RUNTIME_FALLBACK mode=setarch-%s-R\n' \
                "$machine_arch"
        fi
    fi

    if [ -n "$tsan_output" ]; then
        printf '%s\n' "$tsan_output"
    fi

    if [ "$tsan_status" -ne 0 ]; then
        if [ "$tsan_status" -eq 124 ]; then
            printf \
                'udp_publisher_concurrency_tsan=TIMEOUT run=%d exit=%d\n' \
                "$tsan_run" \
                "$tsan_status" >&2
        elif [ "$tsan_status" -eq 66 ]; then
            printf \
                'udp_publisher_concurrency_tsan=TSAN_ERROR run=%d exit=%d\n' \
                "$tsan_run" \
                "$tsan_status" >&2
        else
            printf \
                'udp_publisher_concurrency_tsan=FAIL run=%d exit=%d\n' \
                "$tsan_run" \
                "$tsan_status" >&2
        fi

        exit "$tsan_status"
    fi

    tsan_run=$((tsan_run + 1))
done

printf 'udp_publisher_concurrency_tsan=PASS runs=%d\n' "$tsan_runs"
