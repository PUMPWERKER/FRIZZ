#!/bin/bash
# all.sh: runs every host check on the working tree: the engine against HEAD (check.sh), then
# each unit check (every NAME.cpp but the harness), and doclinks: the docs the website is built
# from (../tools/doclinks.py --all: its checks and its selftest). Prints one line per check and exits 0 when
# all pass, with the faults the checks know of (Known() in check.h) listed under theirs, and
# what a check couldn't run on this machine (its lines "SKIP: local only (what's missing)": stage
# 1b, CLAUDE.md). A
# changed sound (check.sh not bit-identical) counts as a failure: run ./check.sh to
# see what changed. The tools against the twin (remote, midisuite, reaper) run on the twin's
# clock (ptytwin.h's lockstep, #388), their timing the same under any load; with
# FRIZZ_TWIN_CLOCK=0 on the wall clock, where a run that failed with the twin far behind and
# passed when run again is listed as FLAKY under its check (ptytwin.h's Twice(), #183).
#
# The checks run side by side, one per core (JOBS=N sets how many), once DaisySP and the twins
# they share are built; their lines come in the order above. The ones on the twin go first,
# the longest; ui, sync, midi and midiout each run their cases side by side too (cases.h,
# CASE_JOBS). remote.cpp, midisuite.cpp and reaper.cpp, long, and bench.cpp, the longest
# single process, start first and at the normal priority, the others behind them (nice). CASES (for working
# on a few cases) is ignored: all.sh runs every case.
#
# A check that fails is named, with what it printed but its ok lines; one that exits non-zero
# without naming a failing check, was killed (a signal), or left no exit code at all says so
# (#107). Every check's whole output of the last run stays in
# build/all-last/NAME.out.
source "$(dirname "$0")/lib.sh"
set +e
unset CASES
JOBS=${JOBS:-$(nproc)}
OUT=$(mktemp -d)
LAST=$T/build/all-last
trap 'rm -rf "$LAST" && mkdir -p "$T/build" && mv "$OUT" "$LAST"' EXIT
for t in $(units); do
    twin_for "$t" > /dev/null || { echo "FAIL  $t: its twin didn't build"; exit 1; }
done

on_twin=$(for t in $(units); do grep -q '#include "twin.h"' "$T/$t.cpp" && echo "$t"; done)
others=$(for t in check doclinks $(units); do echo "$on_twin" | grep -qx "$t" || echo "$t"; done)
run()
{
    local t=$1 nice=
    echo "$first" | grep -qx "$t" || nice="nice -n 10"
    case $t in
        check) $nice "$T/check.sh" ;;
        doclinks) python3 "$T/../tools/doclinks.py" --all ;;
        *) $nice "$T/unit.sh" "$t" ;;
    esac > "$OUT/$t.out" 2>&1
    echo $? > "$OUT/$t.rc"
}
first=$(printf '%s\n' remote midisuite reaper bench | grep -x -f <(units))
for t in $first $(echo "$on_twin" | grep -vx -f <(echo "$first")) $others; do
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
    run "$t" &
done
wait

failed=
for t in check doclinks $(units); do
    out=$(cat "$OUT/$t.out" 2>/dev/null)
    rc=$(cat "$OUT/$t.rc" 2>/dev/null)
    if [ "$rc" = 0 ]; then
        echo "ok    $t: $(echo "$out" | tail -1)$(echo "$out" | grep -m1 -E '^[0-9]+ known$' | sed 's/^/, /')"
        echo "$out" | grep -E "^(KNOWN|FLAKY|SKIP)" | sed 's/^/      /'
        continue
    fi
    failed="$failed $t"
    if [ -z "$rc" ]; then
        echo "FAIL  $t: no exit code: its run was lost"
    elif [ "$rc" -gt 128 ]; then
        echo "FAIL  $t: killed by SIG$(kill -l $((rc - 128)) 2>/dev/null || echo $((rc - 128)))"
    else
        echo "FAIL  $t: exit $rc"
    fi
    echo "$out" | grep -v "^ok" | sed 's/^/      /'
    echo "$out" | grep -q "FAIL" || echo "      (no failing check named: all it printed is $LAST/$t.out)"
done
if [ -n "$failed" ]; then
    echo "failed:$failed; each one's whole output: $LAST/NAME.out"
    exit 1
fi
