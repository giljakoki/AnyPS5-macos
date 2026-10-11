#!/bin/bash
# Reports on each test ctest recorded as failed: the crash reports of its run under ctest, then the test run
# again alone with its output, the stacks of all its threads (sample) when it still runs after 25 s, and the
# crash report of that run. Usage: diagnose_failed_tests.sh <build dir> <file touched before ctest started>
# No debugger: on the hosted runners lldb waits for an authorization nobody gives, and a process it was
# attaching to could not be killed. Every wait has a time limit, so the report always finishes.
build=$1
since=$2
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

stamp="$build/rerun.stamp"
for name in $(sed -n 's/^[0-9]*://p' "$failed"); do
    echo "===== $name"
    info=$(ctest --test-dir "$build" -R "^${name}\$" -N -V < /dev/null)
    command=$(printf '%s\n' "$info" | sed -n 's/^[0-9]*: Test command: //p' | head -n 1)
    directory=$(printf '%s\n' "$info" | sed -n 's/^[0-9]*: Working Directory: //p' | head -n 1)
    [ -n "$directory" ] || directory=$build
    program=$(printf '%s\n' "$command" | awk '{print $1}')
    echo "command: $command"
    echo "----- under ctest"
    python3 "$here/crash_reports.py" "$since" "$program"

    echo "----- alone"
    touch "$stamp"
    (cd "$directory" && eval "exec $command") > "$build/rerun.log" 2>&1 < /dev/null &
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
    tail -n 80 "$build/rerun.log"
    echo "exit status: $status"
    # A signal other than our SIGKILL ends the test with a crash report, which ReportCrash writes a few seconds later.
    if [ "$status" != killed ] && [ "$status" -gt 128 ] && [ "$status" -ne 137 ]; then
        for second in $(seq 1 20); do
            [ -n "$(find ~/Library/Logs/DiagnosticReports -name '*.ips' -newer "$stamp" 2>/dev/null | head -n 1)" ] && break
            sleep 1
        done
        sleep 2
        python3 "$here/crash_reports.py" "$stamp" "$program"
    fi
done
