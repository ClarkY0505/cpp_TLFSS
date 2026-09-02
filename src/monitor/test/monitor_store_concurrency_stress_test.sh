stress_binary=/tmp/monitor_store_concurrency_stress
stress_runs=200
stress_run=1

while [ "$stress_run" -le "$stress_runs" ]; do
    timeout --kill-after=2s 10s \
        "$stress_binary"

    stress_status=$?

    if [ "$stress_status" -ne 0 ]; then
        if [ "$stress_status" -eq 124 ]; then
            printf \
                'monitor_store_concurrency_stress=TIMEOUT run=%d exit=%d\n' \
                "$stress_run" \
                "$stress_status"
                        else
                            printf \
                                'monitor_store_concurrency_stress=FAIL run=%d exit=%d\n' \
                                "$stress_run" \
                                "$stress_status"
        fi

        exit "$stress_status"
    fi

    stress_run=$((stress_run + 1))
done

printf \
    'monitor_store_concurrency_stress=PASS runs=%d\n' \
    "$stress_runs"

