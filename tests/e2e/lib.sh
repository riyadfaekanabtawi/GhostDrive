# Helpers sourced by the e2e scripts. They expect GD, GDD and the PIDS array to be set.
#
# Ports are picked at runtime because other checkouts may run these tests at the same time. The
# range is below the macOS and Linux ephemeral ranges.

# Prints a UDP port that nothing is bound to right now.
free_port() {
    local p
    for _ in $(seq 50); do
        p=$((20000 + RANDOM % 12000))
        command -v lsof >/dev/null || { echo "$p"; return; }
        lsof -nP -iUDP:"$p" >/dev/null 2>&1 || { echo "$p"; return; }
    done
    return 1
}

# start_daemon DATA MOUNT PORT LOG [ARGS...]: starts a daemon and waits until it answers.
# Returns 2 if its port was taken in the meantime.
start_daemon() {
    local data=$1 mount=$2 port=$3 log=$4
    shift 4
    "$GDD" --data "$data" --mount "$mount" --port "$port" "$@" >"$log" 2>&1 &
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

# start_daemon_any_port DATA MOUNT LOG [ARGS...]: like start_daemon on a free port, retrying with
# another port if one is taken. Sets PORT. GHOSTDRIVE_E2E_FIRST_PORT forces the first attempt's
# port, to test the retry.
start_daemon_any_port() {
    local data=$1 mount=$2 log=$3 started=1
    shift 3
    PORT=${GHOSTDRIVE_E2E_FIRST_PORT:-}
    for _ in $(seq 5); do
        [ -n "$PORT" ] || PORT=$(free_port) || return 1
        start_daemon "$data" "$mount" "$PORT" "$log" "$@"
        started=$?
        [ $started -ne 2 ] && break
        echo "port $PORT was taken, retrying"
        PORT=
    done
    return $started
}
