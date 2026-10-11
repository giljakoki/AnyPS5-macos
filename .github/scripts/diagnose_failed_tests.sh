#!/bin/bash
# Runs each test that ctest recorded as failed again on its own and prints its output, a backtrace when
# it crashes, and the stacks of all its threads when it hangs. Usage: diagnose_failed_tests.sh <build dir>
build=$1
failed="$build/Testing/Temporary/LastTestsFailed.log"
[ -f "$failed" ] || { echo "no failed tests recorded"; exit 0; }
sed -n 's/^[0-9]*://p' "$failed" | while read -r name; do
    echo "===== $name"
    info=$(ctest --test-dir "$build" -R "^${name}\$" -N -V)
    command=$(printf '%s\n' "$info" | sed -n 's/^[0-9]*: Test command: //p' | head -n 1)
    directory=$(printf '%s\n' "$info" | sed -n 's/^[0-9]*: Working Directory: //p' | head -n 1)
    [ -n "$directory" ] || directory=$build
    echo "command: $command"
    (cd "$directory" && eval "exec $command") > "$build/rerun.log" 2>&1 < /dev/null &
    pid=$!
    hung=0
    for second in $(seq 1 60); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    if kill -0 "$pid" 2>/dev/null; then
        hung=1
        echo "still running after 60 s; stacks of all threads:"
        lldb --batch -p "$pid" -o "thread backtrace all" -o "detach" < /dev/null 2>&1 | head -n 400
        kill -9 "$pid" 2>/dev/null
    fi
    wait "$pid"
    status=$?
    tail -n 80 "$build/rerun.log"
    echo "exit status: $status"
    if [ "$hung" = 0 ] && [ "$status" -ne 0 ]; then
        echo "backtrace:"
        (cd "$directory" && eval "lldb --batch -o run -k 'thread backtrace all' -k kill -- $command") < /dev/null 2>&1 | tail -n 150
    fi
done
