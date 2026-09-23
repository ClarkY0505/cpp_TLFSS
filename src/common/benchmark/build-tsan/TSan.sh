#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
benchmark_binary="${script_dir}/benchmark"

if [[ ! -x "${benchmark_binary}" ]]; then
    printf 'TSan benchmark not found: %s\n' "${benchmark_binary}" >&2
    exit 1
fi

export TSAN_OPTIONS=halt_on_error=1

tests=(
    memory-runtime-concurrency-test
    memory-runtime-dynamic-worker-test
    runtime-degradation-test
    lazy-runtime-parallel-test
    concurrent-first-use-test
    runtime-construction-failure-test
    concurrent-construction-failure-test
    parallel-nt-stress
    sparse-worker-stress
    runtime-lifetime-test
)

for test_name in "${tests[@]}"; do
    printf '\n===== %s =====\n' "${test_name}"

    if setarch x86_64 -R "${benchmark_binary}" "${test_name}"; then
        continue
    else
        status=$?
        printf 'FAILED: %s (exit %d)\n' "${test_name}" "${status}" >&2
        exit "${status}"
    fi
done

printf '\nTSan regression: PASS\n'
