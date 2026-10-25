#!/bin/bash
# Launches ghostdrive-app on a virtual X display against throwaway daemons and saves a
# screenshot of each page. Used by CI; needs Xvfb, xdotool and ImageMagick's import.
# Usage: screenshots.sh <build-dir> <output-dir>
#
# Three devices on loopback: desk (the one the app shows), laptop (running) and phone (joined,
# then never started, so it shows as offline). The daemons run with --no-mount.
set -u
BIN=$(cd "$1" && pwd)
OUT=$2
GD="$BIN/cli/ghostdrive"
GDD="$BIN/daemon/ghostdrived"
APP="$BIN/linux/ghostdrive-app/ghostdrive-app"
WORK=$(mktemp -d)
mkdir -p "$OUT"
PIDS=()

cleanup() {
    for d in desk laptop; do "$GD" stop --data "$WORK/$d" >/dev/null 2>&1; done
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*"
    for f in "$WORK"/*.log "$OUT"/*.log; do [ -f "$f" ] && { echo "--- $f"; cat "$f"; }; done
    exit 1
}

wait_for() {
    local limit=$1
    shift
    for _ in $(seq $((limit * 10))); do "$@" >/dev/null 2>&1 && return 0; sleep 0.1; done
    return 1
}

port() { echo $((20000 + RANDOM % 12000)); }

# The app keeps its settings under XDG_CONFIG_HOME; keep them out of the runner's home.
export XDG_CONFIG_HOME="$WORK/config" XDG_DATA_HOME="$WORK/share"

PA=$(port) PB=$(port) PC=$(port)
"$GD" init --data "$WORK/desk" --name desk >/dev/null || fail "init"
"$GDD" --data "$WORK/desk" --no-mount --no-lan --port "$PA" >"$WORK/desk.log" 2>&1 &
PIDS+=($!)
wait_for 10 "$GD" status --data "$WORK/desk" || fail "desk did not start"

CODE=$("$GD" invite --data "$WORK/desk" | grep -o 'gd1-[A-Z0-9-]*') || fail "invite"
"$GD" join "$CODE" --data "$WORK/laptop" --name laptop --port "$PB" >/dev/null || fail "join laptop"
"$GDD" --data "$WORK/laptop" --no-mount --no-lan --port "$PB" >"$WORK/laptop.log" 2>&1 &
PIDS+=($!)
CODE=$("$GD" invite --data "$WORK/desk" | grep -o 'gd1-[A-Z0-9-]*') || fail "invite"
"$GD" join "$CODE" --data "$WORK/phone" --name phone --port "$PC" >/dev/null || fail "join phone"
wait_for 20 sh -c "'$GD' status --data '$WORK/desk' | grep -q 'online   laptop'" ||
    fail "laptop never connected to desk"
"$GD" status --data "$WORK/desk"

Xvfb :99 -screen 0 1280x900x24 -nolisten tcp >"$WORK/xvfb.log" 2>&1 &
PIDS+=($!)
export DISPLAY=:99 GDK_BACKEND=x11 GSK_RENDERER=cairo GTK_A11Y=none NO_AT_BRIDGE=1
wait_for 10 xdpyinfo || fail "Xvfb did not start"

# shot NAME DATA [ARGS...]: starts the app, waits for its window, saves NAME.png, closes it.
shot() {
    local name=$1 data=$2
    shift 2
    "$APP" --data "$data" "$@" >"$OUT/$name.log" 2>&1 &
    local pid=$!
    wait_for 15 xdotool search --name '^GhostDrive$' || fail "$name: no window"
    sleep 4  # a status poll or two, and rendering
    kill -0 "$pid" 2>/dev/null || fail "$name: the app exited"
    local win
    win=$(xdotool search --name '^GhostDrive$' | head -n 1)
    import -window "$win" "$OUT/$name.png" || fail "$name: screenshot"
    kill "$pid"
    wait "$pid" 2>/dev/null
    wait_for 5 sh -c "! xdotool search --name '^GhostDrive\$'" || true
    echo "saved $OUT/$name.png"
}

PATH="$BIN/cli:$PATH"
export PATH
mkdir -p "$WORK/empty"
shot 1-setup "$WORK/empty"
shot 2-home "$WORK/desk" --page home
shot 3-devices "$WORK/desk" --page devices
shot 4-add-device "$WORK/desk" --page add
shot 5-settings "$WORK/desk" --page settings
echo PASS
