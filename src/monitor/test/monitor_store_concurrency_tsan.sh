#!/usr/bin/env bash

set -u
set -o pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)

tsan_runs=${TSAN_RUNS:-10}
tsan_run=1
tsan_timeout_seconds=${TSAN_TIMEOUT_SECONDS:-120}
tsan_kill_after_seconds=${TSAN_KILL_AFTER_SECONDS:-10}
tsan_options=halt_on_error=1:exitcode=66
cxx=${CXX:-g++}

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
    -I"$project_root/inc/monitor"
)

if ! command -v "$cxx" >/dev/null 2>&1; then
    printf 'monitor_store_concurrency_tsan=COMPILER_NOT_FOUND compiler=%s\n' "$cxx" >&2
    exit 1
fi

if ! command -v timeout >/dev/null 2>&1; then
    printf '%s\n' 'monitor_store_concurrency_tsan=TIMEOUT_COMMAND_NOT_FOUND' >&2
    exit 1
fi

# 在编译完整测试前先验证 TSan runtime 能否初始化。
# 某些受控环境或高随机化地址空间会报：
#
#   FATAL: ThreadSanitizer: unexpected memory mapping
#
# 这种错误发生在项目代码执行之前。若逐进程关闭 ASLR 可以解决，
# 后续测试仅通过 setarch -R 启动；不会修改系统全局 ASLR 配置。
tsan_probe_binary=$(mktemp /tmp/monitor_store_tsan_probe.XXXXXX)
tsan_binary=$(mktemp /tmp/monitor_store_concurrency_tsan.XXXXXX)
trap 'rm -f -- "$tsan_probe_binary" "$tsan_binary"' EXIT

if ! printf '%s\n' 'int main() { return 0; }' \
    | "$cxx" "${compiler_flags[@]}" \
        -x c++ - \
        -o "$tsan_probe_binary"; then

    printf '%s\n' 'monitor_store_concurrency_tsan=PROBE_COMPILE_FAIL' >&2
    exit 1
fi

runtime_prefix=()

probe_output=$(TSAN_OPTIONS="$tsan_options" "$tsan_probe_binary" 2>&1)
probe_status=$?

if [ "$probe_status" -ne 0 ]; then
    if [[ "$probe_output" == *"unexpected memory mapping"* ]] \
        && command -v setarch >/dev/null 2>&1; then

        machine_arch=$(uname -m)
        fallback_output=$(
            TSAN_OPTIONS="$tsan_options" \
                setarch "$machine_arch" -R "$tsan_probe_binary" 2>&1
        )
        fallback_status=$?

        if [ "$fallback_status" -eq 0 ]; then
            runtime_prefix=(setarch "$machine_arch" -R)
            printf \
                'monitor_store_concurrency_tsan=TSAN_RUNTIME_FALLBACK mode=setarch-%s-R\n' \
                "$machine_arch"
        else
            printf '%s\n' "$probe_output" >&2
            printf '%s\n' "$fallback_output" >&2
            printf \
                'monitor_store_concurrency_tsan=ENVIRONMENT_UNSUPPORTED exit=%d\n' \
                "$fallback_status" >&2
            exit 77
        fi
    else
        printf '%s\n' "$probe_output" >&2
        printf \
            'monitor_store_concurrency_tsan=TSAN_RUNTIME_FAIL exit=%d\n' \
            "$probe_status" >&2
        exit 77
    fi
fi

if ! "$cxx" "${compiler_flags[@]}" \
    "$project_root/src/monitor/monitor_store.cpp" \
    "$script_dir/monitor_store_concurrency_test.cpp" \
    -o "$tsan_binary"; then

    printf '%s\n' 'monitor_store_concurrency_tsan=COMPILE_FAIL' >&2
    exit 1
fi

while [ "$tsan_run" -le "$tsan_runs" ]; do
    TSAN_OPTIONS="$tsan_options" \
        timeout \
            --kill-after="${tsan_kill_after_seconds}s" \
            "${tsan_timeout_seconds}s" \
            "${runtime_prefix[@]}" \
            "$tsan_binary"

    tsan_status=$?

    if [ "$tsan_status" -ne 0 ]; then
        if [ "$tsan_status" -eq 124 ]; then
            printf \
                'monitor_store_concurrency_tsan=TIMEOUT run=%d exit=%d\n' \
                "$tsan_run" \
                "$tsan_status" >&2
        elif [ "$tsan_status" -eq 66 ]; then
            printf \
                'monitor_store_concurrency_tsan=TSAN_ERROR run=%d exit=%d\n' \
                "$tsan_run" \
                "$tsan_status" >&2
        else
            printf \
                'monitor_store_concurrency_tsan=FAIL run=%d exit=%d\n' \
                "$tsan_run" \
                "$tsan_status" >&2
        fi

        exit "$tsan_status"
    fi

    tsan_run=$((tsan_run + 1))
done

printf \
    'monitor_store_concurrency_tsan=PASS runs=%d\n' \
    "$tsan_runs"
