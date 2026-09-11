#!/usr/bin/env bash
set -euo pipefail

rm -f alarms.log stats.bin
./caredispatch > test-output.txt 2>&1 &
server_pid=$!
trap 'kill -TERM "$server_pid" 2>/dev/null || true' EXIT
sleep 0.4
./send_alarm SERVICE Erik "Needs help with dishes"
./send_alarm FALL Anna "Fell in the bathroom"
./send_alarm MEDICINE Fatima "Missed evening medicine"
sleep 1
kill -TERM "$server_pid"
wait "$server_pid"
trap - EXIT

grep -q "Received: 3" test-output.txt
grep -q "Completed: 3" test-output.txt
test "$(grep -c 'COMPLETED' alarms.log)" -eq 3
echo "All integration tests passed."
