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
# Ports are picked at runtime: other checkouts may run this test at the same time. The range is
# below the macOS and Linux ephemeral ranges.
free_port() {
    local p
    for _ in $(seq 50); do
        p=$((20000 + RANDOM % 12000))
        command -v lsof >/dev/null || { echo "$p"; return; }
        lsof -nP -iUDP:"$p" >/dev/null 2>&1 || { echo "$p"; return; }
    done
    return 1
}

# Starts a daemon and waits until it answers. Returns 2 if its port was taken in the meantime.
start_daemon() {
    local data=$1 mount=$2 port=$3 log=$4
    "$GDD" --data "$data" --mount "$mount" --port "$port" >"$log" 2>&1 &
    local pid=$!
    PIDS+=($pid)
    for _ in $(seq 100); do
        "$GD" status --data "$data" >/dev/null 2>&1 && return 0
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    grep -q "Address already in use" "$log" && return 2
    return 1
}

for _ in $(seq 5); do
    PORT_A=$(free_port) || fail "no free UDP port"
    start_daemon "$A" "$MA" "$PORT_A" "$WORK/a.log"
    started=$?
    [ $started -ne 2 ] && break
done
[ $started -eq 0 ] || fail "daemon A did not start"

CODE=$("$GD" invite --data "$A" | grep -o 'gd1-[A-Z0-9-]*') || fail "invite"
PORT_B=$(free_port) || fail "no free UDP port"
"$GD" join "$CODE" --data "$B" --name laptop --port "$PORT_B" || fail "join"
"$GDD" --data "$B" --mount "$MB" --port "$PORT_B" >"$WORK/b.log" 2>&1 &
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

# Removing B from A cuts it off, and B is told who removed it.
"$GD" remove desk --data "$A" >/dev/null 2>&1 && fail "A removed itself"
"$GD" remove laptop --data "$A" | grep -q "Removed laptop" || fail "remove laptop"
wait_for 15 sh -c "'$GD' status --data '$A' | grep -q 'none yet'" || fail "A still lists B"
wait_for 15 sh -c "'$GD' status --data '$B' | grep -q 'Removed:   desk removed this device'" \
    || fail "B was not told it was removed"
sleep 3
"$GD" status --data "$B" | grep -q "online   desk" && fail "B reconnected after removal"

"$GD" status --data "$A"
echo "PASS"
