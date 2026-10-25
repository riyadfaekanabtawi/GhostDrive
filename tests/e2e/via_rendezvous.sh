#!/bin/bash
# End to end through the rendezvous server: the real ghostdrive-rendezvous binary and two
# daemons with LAN discovery off, whose stored addresses for each other are dead, so only the
# rendezvous can connect them.
# Usage: via_rendezvous.sh <build-dir> <scratch-dir>
set -u
BIN="$1"
WORK="$2"
GD="$BIN/cli/ghostdrive"
GDD="$BIN/daemon/ghostdrived"
A="$WORK/a" B="$WORK/b" C="$WORK/c" MA="$WORK/mnt-a" MB="$WORK/mnt-b"
PIDS=()
source "$(dirname "$0")/lib.sh"

cleanup() {
    [ -n "${RDV:-}" ] && kill "$RDV" 2>/dev/null
    "$GD" stop --data "$A" >/dev/null 2>&1
    "$GD" stop --data "$B" >/dev/null 2>&1
    "$GD" stop --data "$C" >/dev/null 2>&1
    for p in "${PIDS[@]}"; do
        for _ in $(seq 150); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
        kill "$p" 2>/dev/null
    done
    cleanup_mount_parent
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*"
    for log in a b c a2 b2 c2; do
        [ -f "$WORK/$log.log" ] && { echo "--- daemon log $log"; cat "$WORK/$log.log"; }
    done
    exit 1
}

# Waits up to $1 seconds for the command in the remaining arguments to succeed.
wait_for() {
    local limit=$1; shift
    for _ in $(seq $((limit * 10))); do "$@" >/dev/null 2>&1 && return 0; sleep 0.1; done
    return 1
}

rm -rf "$WORK" && mkdir -p "$WORK"
# Port 0: the server binds a port the system picks and prints it.
"$BIN/rendezvous/ghostdrive-rendezvous" --key "$WORK/server.key" --port 0 >"$WORK/server.log" 2>&1 &
RDV=$!
wait_for 10 grep -q Fingerprint "$WORK/server.log" || fail "rendezvous server did not start"
FPR=$(awk '/^Fingerprint:/{print $2}' "$WORK/server.log")
RDV_PORT=$(awk '/listening on UDP port/{print $NF}' "$WORK/server.log")
"$GD" init --data "$A" --name desk >/dev/null || fail "init"
"$GD" rendezvous 127.0.0.1:"$RDV_PORT" "$FPR" --data "$A" >/dev/null || fail "set rendezvous"
start_daemon_any_port "$A" "$MA" "$WORK/a.log" --no-lan || fail "daemon A did not start"
PORT_A=$PORT

CODE=$("$GD" invite --data "$A" | grep -o 'gd1-[A-Z0-9-]*') || fail "invite"
PORT_B=$(free_port) || fail "no free UDP port"
"$GD" join "$CODE" --data "$B" --name laptop --port "$PORT_B" || fail "join"
"$GD" rendezvous --data "$B" | grep -q ":$RDV_PORT" || fail "the invite did not carry the rendezvous setting"
# Kill the address learned from the invite: only the rendezvous can connect the devices now.
# -i.bak works with both BSD and GNU sed.
sed -i.bak -E "s/ [0-9.]+:$PORT_A\$/ 127.0.0.1:9/" "$B/peers" && rm -f "$B/peers.bak"
grep -q " 127.0.0.1:9$" "$B/peers" || fail "could not replace A's address in B's peers"
"$GDD" --data "$B" --mount "$MB" --port "$PORT_B" --no-lan >"$WORK/b.log" 2>&1 &
PIDS+=($!)
wait_for 20 sh -c "'$GD' status --data '$B' | grep -q 'online   desk'" || fail "B never connected to A"
"$GD" status --data "$B" | grep -q "Rendezvous: 127.0.0.1:$RDV_PORT (connected" || fail "B is not registered with the server"

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

# The operator story for --allow-group: a third device joins and is removed, the members move
# to a new discovery key, and once they all hold it the operator lists only the new admission
# ID. The members get in again; the removed device, which has only the old ID, does not.
CODE=$("$GD" invite --data "$A" | grep -o 'gd1-[A-Z0-9-]*') || fail "second invite"
PORT_C=$(free_port) || fail "no free UDP port"
"$GD" join "$CODE" --data "$C" --name spare --port "$PORT_C" >/dev/null || fail "join C"
"$GDD" --data "$C" --no-mount --port "$PORT_C" --no-lan >"$WORK/c.log" 2>&1 &
PID_C=$!
PIDS+=($PID_C)
wait_for 20 sh -c "'$GD' status --data '$C' | grep -q 'Rendezvous: 127.0.0.1:$RDV_PORT (connected'" ||
    fail "C never registered with the server"
"$GD" status --data "$A" | grep -q "^Admission: .* (generation 0)" || fail "A did not start at generation 0"

"$GD" remove spare --data "$A" >/dev/null || fail "remove C"
wait_for 30 sh -c "'$GD' status --data '$A' | grep -q 'all members have the current key'" ||
    fail "A never saw every member holding the new key"
"$GD" status --data "$A" | grep -q "^Admission: .* (generation [1-9]" || fail "A's admission ID did not rotate"
NEW=$("$GD" status --data "$A" | awk '/^Admission:/{print $2}')
wait_for 10 sh -c "'$GD' status --data '$B' | grep -q '^Admission: $NEW '" || fail "B shows another admission ID"

kill "$RDV" 2>/dev/null
for _ in $(seq 50); do kill -0 "$RDV" 2>/dev/null || break; sleep 0.1; done
"$BIN/rendezvous/ghostdrive-rendezvous" --key "$WORK/server.key" --port "$RDV_PORT" \
    --allow-group "$NEW" >"$WORK/server2.log" 2>&1 &
RDV=$!
wait_for 10 grep -q Fingerprint "$WORK/server2.log" || fail "the rendezvous server did not restart"
# A daemon notices a vanished server only by idle timeout; restarting the daemons is quicker.
for d in "$A" "$B" "$C"; do "$GD" stop --data "$d" >/dev/null 2>&1; done
for p in "${PIDS[@]}"; do
    for _ in $(seq 150); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
done
PIDS=()
start_daemon "$A" "$MA" "$PORT_A" "$WORK/a2.log" --no-lan || fail "daemon A did not restart"
start_daemon "$B" "$MB" "$PORT_B" "$WORK/b2.log" --no-lan || fail "daemon B did not restart"
"$GDD" --data "$C" --no-mount --port "$PORT_C" --no-lan >"$WORK/c2.log" 2>&1 &
PIDS+=($!)
for d in "$A" "$B"; do
    wait_for 20 sh -c "'$GD' status --data '$d' | grep -q 'Rendezvous: 127.0.0.1:$RDV_PORT (connected'" ||
        fail "a member was refused by the server that lists only the new admission ID"
done
wait_for 10 sh -c "'$GD' status --data '$C' | grep -q 'Rendezvous: 127.0.0.1:$RDV_PORT (not connected'" ||
    fail "C's daemon did not come back"
sleep 3
"$GD" status --data "$C" | grep -q "Rendezvous: 127.0.0.1:$RDV_PORT (connected" &&
    fail "the removed device was admitted with the old admission ID"

"$GD" status --data "$A"
echo "PASS"
