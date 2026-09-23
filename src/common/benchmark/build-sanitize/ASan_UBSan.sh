export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

tests=(
    copy-policy-test
    copy-executor-test
    tlss-memcpy-auto-test
    tlss-memcpy-backend-test
    memory-runtime-concurrency-test
    memory-runtime-dynamic-worker-test
    runtime-degradation-test
    lazy-runtime-test
    lazy-runtime-parallel-test
    concurrent-first-use-test
    runtime-construction-failure-test
    concurrent-construction-failure-test
    parallel-nt-stress
    sparse-worker-stress
    runtime-lifetime-test
)

for test_name in "${tests[@]}"; do
    echo
    echo "===== ${test_name} ====="

    ./benchmark "${test_name}" || {
        echo "FAILED: ${test_name}"
        exit 1
    }
done

echo
echo "ASan + UBSan full regression: PASS"
