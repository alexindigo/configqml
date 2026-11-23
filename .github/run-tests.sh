#!/usr/bin/env bash
set -euo pipefail

build=$1
sysctl kernel.sched_schedstats
test "$(sysctl -n kernel.sched_schedstats)" = 1
ctest --test-dir "$build" --output-on-failure --no-tests=error

# CTest passes when individual QtTest rows skip and hides passing test output.
# Keep both the measurements and the proof that the controls actually ran.
log=$build/Testing/Temporary/LastTest.log
for row in blocked-rejected scheduling-excluded legacy-rejects-scheduling-control; do
    name="ThreadWorkTest::realWorkAndScheduling($row)"
    receipt=$(grep -F "$name" "$log")
    printf '%s\n' "$receipt"
    grep -Eq '^PASS[[:blank:]]*:' <<< "$receipt"
    grep -Eq 'CALLER_WORK .*runqueue_ns=[0-9]+ blocked_ns=[0-9]+ .*valid=1$' <<< "$receipt"
done
