#!/bin/bash
# hostlock.sh: the host lock (CLAUDE.md, "Several sessions on one machine"), which the heavy
# host runs take themselves (#439): all.sh, check.sh, ../twin/compare.sh, ../twin/ui-at.sh,
# ../twin/build.sh run by hand and ../tools/builds.py. Parallel ones have run the machine out of
# memory, so they go one at a time, each under a memory cap:
#
#   source "$(dirname "$0")/hostlock.sh"   the script's first line, before anything heavy: it
#                                          runs the script again under the lock
#   hostlock.sh CMD [ARG...]               CMD under it (builds.py: execs itself through it)
#
# Under the lock means: flock on ~/.cache/frizz-host.lock (FRIZZ_HOST_LOCK another file),
# waiting while another run holds it and saying which (the line the holder wrote into the
# file, or for a holder that wrote none, a hand's `flock FILE ...`, its command), then in a
# systemd user scope with MemoryMax=4G (FRIZZ_HOST_MEMORY another cap, "none" none) and no swap,
# where systemd-run --user works (else without the cap, saying so). FRIZZ_HOST_LOCKED is set
# inside, so what the run starts (all.sh's check.sh, compare.sh's build.sh) goes on without
# waiting for its own lock; a run whose caller holds the lock by hand (`flock FILE systemd-run
# ... ./all.sh`, the way before #439: an ancestor's lock in /proc/locks) doesn't wait for it
# either. Nothing of it with CI set (Actions: a machine of its own) or FRIZZ_HOST_LOCKED set.
# unit.sh doesn't take it: a single check may run beside a heavy run (it sets FRIZZ_HOST_LOCKED
# for what it starts, its twin's build among them).

# whether a process above this one holds a flock on FILE (/proc/locks: its inode, the holder's
# pid; a waiter's line has "->" in its second field)
_frizz_lock_above()
{
    local ino holders p h
    ino=$(stat -L -c %i "$1" 2> /dev/null) || return 1
    holders=$(awk -v ino="$ino" '$2 == "FLOCK" { n = split($6, a, ":"); if (a[n] == ino) print $5 }' /proc/locks 2> /dev/null)
    [ -n "$holders" ] || return 1
    p=$PPID
    while [ -n "$p" ] && [ "$p" -gt 1 ]; do
        for h in $holders; do
            [ "$h" = "$p" ] && return 0
        done
        p=$(awk '/^PPid:/ { print $2 }' "/proc/$p/status" 2> /dev/null)
    done
    return 1
}

# who holds FILE: the line its holder wrote, or the commands of the processes /proc/locks names
_frizz_lock_holder()
{
    local who ino h pid
    who=$(head -c 300 "$1" 2> /dev/null | head -1)
    # a line left by a run that ended isn't the holder's (one by hand writes none)
    pid=$(echo "$who" | sed -n 's/^pid \([0-9]*\) .*/\1/p')
    [ -n "$pid" ] && kill -0 "$pid" 2> /dev/null || who=
    if [ -z "$who" ]; then
        ino=$(stat -L -c %i "$1" 2> /dev/null)
        for h in $(awk -v ino="$ino" '$2 == "FLOCK" { n = split($6, a, ":"); if (a[n] == ino) print $5 }' /proc/locks 2> /dev/null); do
            who="$who${who:+; }pid $h: $(tr '\0' ' ' < "/proc/$h/cmdline" 2> /dev/null | head -c 200)"
        done
    fi
    echo "${who:-?}"
}

# CMD ARG... under the lock: execs it, or returns (0) when there's nothing to take
_frizz_host_lock()
{
    [ -z "$FRIZZ_HOST_LOCKED" ] && [ -z "$CI" ] || return 0
    local lock=${FRIZZ_HOST_LOCK:-$HOME/.cache/frizz-host.lock} mem=${FRIZZ_HOST_MEMORY:-4G} fd cap=()
    mkdir -p "$(dirname "$lock")"
    if _frizz_lock_above "$lock"; then
        export FRIZZ_HOST_LOCKED=above
        return 0
    fi
    exec {fd}>> "$lock" || { echo "hostlock.sh: can't open $lock" >&2; exit 2; }
    if ! flock -n "$fd"; then
        echo "the host lock is held ($(_frizz_lock_holder "$lock")): waiting ($lock)" >&2
        flock "$fd"
    fi
    echo "pid $$ in $PWD since $(date +%H:%M:%S): $*" > "$lock"
    export FRIZZ_HOST_LOCKED=$$
    # the memory cap: systemd-run --user, where it works (expanding no $ in the arguments, from
    # systemd 254 on; before, without)
    if [ "$mem" != none ] && command -v systemd-run > /dev/null; then
        local s=(systemd-run --user --scope --quiet -p MemoryMax="$mem" -p MemorySwapMax=0)
        if "${s[@]}" --expand-environment=no true > /dev/null 2>&1; then
            cap=("${s[@]}" --expand-environment=no)
        elif "${s[@]}" true > /dev/null 2>&1; then
            cap=("${s[@]}")
        fi
    fi
    [ "$mem" = none ] || [ ${#cap[@]} -gt 0 ] || echo "hostlock.sh: no systemd-run --user here: no memory cap" >&2
    exec "${cap[@]}" "$@"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    [ $# -gt 0 ] || { echo "usage: hostlock.sh CMD [ARG...]"; exit 2; }
    _frizz_host_lock "$@"
    exec "$@"
fi
_frizz_host_lock "$BASH" "$(readlink -f "$0")" "$@"
