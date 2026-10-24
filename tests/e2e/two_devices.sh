#!/bin/bash
# End to end on one machine: two daemons, two FUSE mounts, pairing by invite code, and files
# written through one mount read back through the other.
# Usage: two_devices.sh <build-dir> <scratch-dir>
set -u
BIN="$1"
WORK="$2"
GD="$BIN/cli/ghostdrive"
GDD="$BIN/daemon/ghostdrived"
A="$WORK/a" B="$WORK/b" MA="$WORK/mnt-a" MB="$WORK/mnt-b"
PIDS=()

cleanup() {
    "$GD" stop --data "$A" >/dev/null 2>&1
    "$GD" stop --data "$B" >/dev/null 2>&1
    for p in "${PIDS[@]}"; do
        for _ in $(seq 150); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
        kill "$p" 2>/dev/null
    done
}
trap cleanup EXIT

fail() { echo "FAIL: $*"; echo "--- daemon A log"; cat "$WORK/a.log"; echo "--- daemon B log"; cat "$WORK/b.log" 2>/dev/null; exit 1; }

# Waits up to $1 seconds for the command in the remaining arguments to succeed.
wait_for() {
    local limit=$1; shift
    for _ in $(seq $((limit * 10))); do "$@" >/dev/null 2>&1 && return 0; sleep 0.1; done
    return 1
}

rm -rf "$WORK" && mkdir -p "$WORK"
"$GD" init --data "$A" --name desk >/dev/null || fail "init"
"$GDD" --data "$A" --mount "$MA" --port 47301 >"$WORK/a.log" 2>&1 &
PIDS+=($!)
wait_for 10 "$GD" status --data "$A" || fail "daemon A did not start"

CODE=$("$GD" invite --data "$A" | grep -o 'gd1-[A-Z0-9-]*') || fail "invite"
"$GD" join "$CODE" --data "$B" --name laptop --port 47302 || fail "join"
"$GDD" --data "$B" --mount "$MB" --port 47302 >"$WORK/b.log" 2>&1 &
PIDS+=($!)
wait_for 15 sh -c "'$GD' status --data '$B' | grep -q 'online   desk'" || fail "B never connected to A"

echo "hello from the desk" >"$MA/greeting.txt" || fail "write through A's mount"
head -c 3000000 /dev/urandom >"$WORK/big.bin"
cp "$WORK/big.bin" "$MA/big.bin" || fail "copy into A's mount"
mkdir -p "$MA/photos/2026" && echo "beach" >"$MA/photos/2026/beach.txt"

wait_for 15 cmp -s "$MB/big.bin" "$WORK/big.bin" || fail "big.bin never arrived intact on B"
wait_for 15 grep -qx "hello from the desk" "$MB/greeting.txt" || fail "greeting.txt differs on B"
wait_for 15 grep -qx "beach" "$MB/photos/2026/beach.txt" || fail "nested file missing on B"

echo "reply from the laptop" >"$MB/reply.txt"
wait_for 15 grep -q "reply from the laptop" "$MA/reply.txt" || fail "reply never reached A"

mv "$MA/greeting.txt" "$MA/hello.txt"
wait_for 15 test -f "$MB/hello.txt" || fail "rename never reached B"
wait_for 15 sh -c "! test -e '$MB/greeting.txt'" || fail "old name still on B"
rm "$MB/reply.txt"
wait_for 15 sh -c "! test -e '$MA/reply.txt'" || fail "delete never reached A"

"$GD" status --data "$A"
echo "PASS"
