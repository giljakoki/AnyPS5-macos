#!/bin/bash
# Reports on each test ctest recorded as failed: it runs the test again alone and prints its output, how long
# it took, the stack when it aborts (abort_backtrace.cpp, a library inserted into the test, resolved to lines
# by resolve_backtrace.py) and the stacks of all its threads (sample) when it still runs after 25 s. It first
# prints how late 5 ms timed waits and 1 ms sleeps end on this runner at default QoS, user-interactive QoS and
# with the task at latency tier 0 (timer_probe.cpp), for the tests that measure time.
# Usage: diagnose_failed_tests.sh <build dir>
# No debugger: on the hosted runners lldb waits for an authorization nobody gives, and a process it was
# attaching to could not be killed; the runners write no crash reports. Every wait has a time limit.
build=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
failed="$build/Testing/Temporary/LastTestsFailed.log"
[ -f "$failed" ] || { echo "no failed tests recorded"; exit 0; }

# Waits for a background process for at most $2 seconds; kills it and its children when it overruns, and
# gives up on a process that does not end within 10 s of being killed. Returns 1 when it overran.
finish_within() {
    local pid=$1 seconds=$2 waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$seconds" ]; do sleep 1; waited=$((waited + 1)); done
    kill -0 "$pid" 2>/dev/null || return 0
    pkill -9 -P "$pid" 2>/dev/null
    kill -9 "$pid" 2>/dev/null
    waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 10 ]; do sleep 1; waited=$((waited + 1)); done
    kill -0 "$pid" 2>/dev/null && echo "(process $pid did not end after SIGKILL)"
    return 1
}

tools="$build/diagnose-tools"
mkdir -p "$tools"
shim=
if clang++ -arch x86_64 -std=c++17 -dynamiclib -o "$tools/abort_backtrace.dylib" "$here/abort_backtrace.cpp" > "$tools/build.log" 2>&1; then
    shim="$tools/abort_backtrace.dylib"
else
    cat "$tools/build.log"
fi
echo "----- timer precision"
if clang++ -arch x86_64 -std=c++17 -O1 -o "$tools/timer_probe" "$here/timer_probe.cpp" >> "$tools/build.log" 2>&1; then
    "$tools/timer_probe" < /dev/null > "$tools/timer.log" 2>&1 &
    finish_within $! 60 || echo "(timer probe did not finish within 60 s)"
    cat "$tools/timer.log"
else
    cat "$tools/build.log"
fi

for name in $(sed -n 's/^[0-9]*://p' "$failed"); do
    echo "===== $name"
    info=$(ctest --test-dir "$build" -R "^${name}\$" -N -V < /dev/null)
    command=$(printf '%s\n' "$info" | sed -n 's/^[0-9]*: Test command: //p' | head -n 1)
    directory=$(printf '%s\n' "$info" | sed -n 's/^[0-9]*: Working Directory: //p' | head -n 1)
    [ -n "$directory" ] || directory=$build
    program=$(printf '%s\n' "$command" | awk '{print $1}')
    echo "command: $command"
    started=$SECONDS
    (cd "$directory" && { [ -z "$shim" ] || export DYLD_INSERT_LIBRARIES="$shim"; } && eval "exec $command") > "$build/rerun.log" 2>&1 < /dev/null &
    pid=$!
    for second in $(seq 1 25); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    if kill -0 "$pid" 2>/dev/null; then
        echo "still running after 25 s; stacks of all threads:"
        sudo -n /usr/bin/sample "$pid" 5 -mayDie -file "$build/sample.txt" < /dev/null > "$build/sample.log" 2>&1 &
        finish_within $! 90 || echo "(sample did not finish within 90 s)"
        if [ -s "$build/sample.txt" ]; then head -n 500 "$build/sample.txt"; else tail -n 20 "$build/sample.log"; fi
        rm -f "$build/sample.txt"
    fi
    status=killed
    if finish_within "$pid" 0; then
        wait "$pid" 2>/dev/null
        status=$?
    fi
    tail -n 120 "$build/rerun.log"
    echo "exit status: $status after $((SECONDS - started)) s"
    python3 "$here/resolve_backtrace.py" "$program" "$build/rerun.log"
done
