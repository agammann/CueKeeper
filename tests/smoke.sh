#!/bin/sh

# Run from the project root on the machine that runs both binaries.
export LC_ALL=C

if [ "$#" -gt 1 ]; then
    printf 'Usage %s [binary-directory]\n' "$0" >&2
    exit 2
fi

bin_dir=${1:-build/qnx-x86_64}
engine=$bin_dir/cuekeeper-engine
controller=$bin_dir/cuekeeperctl

if [ ! -x "$engine" ] || [ ! -x "$controller" ]; then
    printf 'Build cuekeeper-engine and cuekeeperctl in %s first.\n' "$bin_dir" >&2
    exit 2
fi

for utility in cat cmp mkdir rm rmdir sleep; do
    if ! command -v "$utility" >/dev/null 2>&1; then
        printf 'The smoke test requires %s on the test target.\n' "$utility" >&2
        exit 2
    fi
done

# Some QNX images map /tmp to /dev/shmem, which cannot create directories.
umask 077
tmpdir=/tmp/cuekeeper-smoke.$$
if ! mkdir "$tmpdir" 2>/dev/null; then
    tmpdir=./.cuekeeper-smoke.$$
    mkdir "$tmpdir" || exit 1
fi
export CUEKEEPER_SOCKET=$tmpdir/engine.sock
engine_pid=

cleanup() {
    if [ -n "$engine_pid" ]; then
        if kill "$engine_pid" 2>/dev/null; then
            sleep 1
            kill -KILL "$engine_pid" 2>/dev/null || :
        fi
        wait "$engine_pid" 2>/dev/null || :
    fi
    rm -f "$tmpdir/engine.sock" "$tmpdir/engine.out" "$tmpdir/engine.err" \
        "$tmpdir/initial.out" "$tmpdir/session.out" "$tmpdir/session.err" \
        "$tmpdir/busy.out" "$tmpdir/busy.err" "$tmpdir/first-cue" \
        "$tmpdir/complete.out" "$tmpdir/repeated.out"
    rmdir "$tmpdir"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM

fail() {
    printf 'FAIL %s\n' "$*" >&2
    for file in "$tmpdir/"*; do
        if [ -f "$file" ] && [ -s "$file" ]; then
            printf '\n--- %s ---\n' "${file##*/}" >&2
            cat "$file" >&2
        fi
    done
    exit 1
}

run_controller() {
    limit=$1
    shift
    # An asynchronous shell command otherwise starts with stdin on /dev/null.
    (
        # Let ksh enter wait before a fast one-shot client can finish.
        sleep 1
        exec "$controller" "$@"
    ) <&3 3<&- &
    controller_pid=$!
    (
        elapsed=0
        while kill -0 "$controller_pid" 2>/dev/null; do
            if [ "$elapsed" -eq "$limit" ]; then
                kill -KILL "$controller_pid" 2>/dev/null || :
                break
            fi
            sleep 1
            elapsed=$((elapsed + 1))
        done
    ) 3<&- &
    watchdog_pid=$!
    wait "$controller_pid"
    controller_status=$?
    wait "$watchdog_pid" 2>/dev/null || :
    return "$controller_status"
} 3<&0

"$engine" >"$tmpdir/engine.out" 2>"$tmpdir/engine.err" &
engine_pid=$!

startup_wait=0
while [ ! -s "$tmpdir/engine.out" ]; do
    kill -0 "$engine_pid" 2>/dev/null || fail 'Engine exited before becoming ready.'
    [ "$startup_wait" -lt 8 ] || fail 'Engine did not become ready within 8 seconds.'
    startup_wait=$((startup_wait + 1))
    sleep 1
done
IFS= read -r ready <"$tmpdir/engine.out" || fail 'Engine readiness line is incomplete.'
[ "$ready" = "$(printf 'READY\tcuekeeper')" ] || fail 'Engine readiness line is unexpected.'
kill -0 "$engine_pid" 2>/dev/null || fail 'Engine exited after reporting readiness.'

run_controller 5 status >"$tmpdir/initial.out" || fail 'Initial Status failed.'
[ "$(cat "$tmpdir/initial.out")" = 'scene=opening state=idle duration_ms=30000 history_count=0' ] ||
    fail 'The test engine did not start with an idle scene and empty history.'

first_cue_seen() {
    snapshot=
    cue=
    while IFS= read -r line; do
        case "$line" in
            scene=*) snapshot=$line; cue= ;;
            cue=*) cue=${line%% *} ;;
        esac
    done <"$tmpdir/session.out"
    [ "$snapshot" = 'scene=opening state=active duration_ms=30000 history_count=1' ] &&
        [ "$cue" = 'cue=lx-1' ]
}

# Keep the controller that sent Go open until it reports exactly the first cue.
drive_controller() {
    printf 'go\n' || return 1
    response_wait=0
    while [ ! -s "$tmpdir/session.out" ]; do
        [ "$response_wait" -lt 5 ] || return 1
        response_wait=$((response_wait + 1))
        sleep 1
    done
    IFS= read -r started <"$tmpdir/session.out" || return 1
    [ "$started" = 'scene=opening state=active duration_ms=30000 history_count=0' ] || return 1

    run_controller 5 go >"$tmpdir/busy.out" 2>"$tmpdir/busy.err"
    busy_status=$?
    [ "$busy_status" -eq 2 ] || return 1
    case "$(cat "$tmpdir/busy.err")" in
        *'scene is already active'*) ;;
        *) return 1 ;;
    esac

    poll=0
    while [ "$poll" -lt 12 ]; do
        printf 'status\n' || return 1
        sleep 1
        if first_cue_seen; then
            printf 'quit\n' || return 1
            : >"$tmpdir/first-cue"
            return 0
        fi
        poll=$((poll + 1))
    done
    return 1
}

drive_controller | run_controller 22 >"$tmpdir/session.out" 2>"$tmpdir/session.err" ||
    fail 'Interactive controller did not exit cleanly.'
[ -f "$tmpdir/first-cue" ] || fail 'Go, active Go rejection, or first cue observation failed.'
printf 'Controller exited after lx-1. Waiting for the scene to finish.\n'

# Scene duration is 30 seconds and lx-1 was observed at or after 5 seconds.
# Send no requests during the remainder of the scene.
sleep 26
kill -0 "$engine_pid" 2>/dev/null || fail 'Engine exited while the controller was disconnected.'
run_controller 5 status >"$tmpdir/complete.out" || fail 'Reconnect Status failed.'
run_controller 5 status >"$tmpdir/repeated.out" || fail 'Repeated Status failed.'
cmp -s "$tmpdir/complete.out" "$tmpdir/repeated.out" || fail 'Status changed the completed snapshot.'

parse_milliseconds() {
    whole=${1%.*}
    fraction=${1##*.}
    [ "$1" = "$whole.$fraction" ] || return 1
    case "$whole" in ''|*[!0-9]*|0[0-9]*) return 1 ;; esac
    case "$fraction" in
        [0-9][0-9][0-9]) ;;
        *) return 1 ;;
    esac
    # Prefix the fraction so shell arithmetic handles leading zeroes as decimal.
    microseconds=$((whole * 1000 + 1$fraction - 1000))
}

check_record() {
    IFS=' ' read -r cue scheduled actual lateness extra || return 1
    [ "$cue" = "cue=$1" ] && [ "$scheduled" = "scheduled_ms=$2" ] &&
        [ -z "$extra" ] || return 1
    case "$actual $lateness" in actual_ms=*\ lateness_ms=*) ;; *) return 1 ;; esac

    parse_milliseconds "${actual#actual_ms=}" || return 1
    actual_us=$microseconds
    parse_milliseconds "${lateness#lateness_ms=}" || return 1
    elapsed_us=$((actual_us - $2 * 1000))
    rounding_us=$((elapsed_us - microseconds))
    [ "$actual_us" -ge "$previous_actual_us" ] && [ "$elapsed_us" -ge 0 ] &&
        [ "$rounding_us" -ge -1 ] && [ "$rounding_us" -le 1 ] || return 1
    previous_actual_us=$actual_us
}

history_matches() {
    IFS= read -r snapshot || return 1
    [ "$snapshot" = 'scene=opening state=complete duration_ms=30000 history_count=3' ] || return 1
    previous_actual_us=0
    check_record lx-1 5000 && check_record snd-1 15000 && check_record cap-1 25000 || return 1
    ! IFS= read -r extra && [ -z "$extra" ]
}

dispatches_match() {
    count=0
    while IFS=' ' read -r event cue output value; do
        [ "$event" = dispatch ] || continue
        count=$((count + 1))
        case "$count $cue $output $value" in
            '1 cue=lx-1 output=lighting value='?* | \
            '2 cue=snd-1 output=sound value='?* | \
            '3 cue=cap-1 output=caption value='?*) ;;
            *) return 1 ;;
        esac
    done
    [ "$count" -eq 3 ]
}

history_matches <"$tmpdir/complete.out" || fail 'History must show each cue in order with valid dispatch timing.'
dispatches_match <"$tmpdir/engine.out" || fail 'The engine must dispatch each simulated output once and in order.'

cat "$tmpdir/complete.out"
printf 'PASS Active Go rejected, three outputs dispatched once in order, reconnect history unchanged.\n'
