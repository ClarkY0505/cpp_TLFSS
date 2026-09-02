asan_binary=/tmp/monitor_store_concurrency_asan
asan_runs=20
asan_run=1

g++ -std=c++17 \
    -O1 \
    -g \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    -pthread \
    monitor_store.cpp \
    test/monitor_store_concurrency_test.cpp \
    -o /tmp/monitor_store_concurrency_asan


while [ "$asan_run" -le "$asan_runs" ]; do
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        timeout --kill-after=5s 60s \
        "$asan_binary"

    asan_status=$?

    if [ "$asan_status" -ne 0 ]; then
        if [ "$asan_status" -eq 124 ]; then
            printf \
                'monitor_store_concurrency_asan=TIMEOUT run=%d exit=%d\n' \
                "$asan_run" \
                "$asan_status"
                        else
                            printf \
                                'monitor_store_concurrency_asan=FAIL run=%d exit=%d\n' \
                                "$asan_run" \
                                "$asan_status"
        fi

        exit "$asan_status"
    fi

    asan_run=$((asan_run + 1))
done

printf \
    'monitor_store_concurrency_asan=PASS runs=%d\n' \
    "$asan_runs"

